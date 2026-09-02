#include "ncurses.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>

WINDOW  stdscr_obj;
WINDOW *stdscr = &stdscr_obj;
int COLS = 80, LINES = 24;

// 256 is what the terminal's palette holds (userland/term/palette.c).
// The viewer checks its material count against both.
int COLORS = 256, COLOR_PAIRS = 256;

static struct termios saved_tio;
static int  tio_saved;
static int  read_timeout = -1;          // -1 blocking, 0 poll, >0 ms

// Components are kept in ncurses' own 0..1000 range, not converted on
// the way in: color_content has to hand back what init_color was given,
// and a round trip through 0..255 would not return the same numbers.
// The conversion to what the terminal wants happens at attron() time.
static short col_r[256], col_g[256], col_b[256];
static short pair_fg[256];

static void query_size(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        COLS  = ws.ws_col;
        LINES = ws.ws_row;
    }
}

int initscr_shim(void) {
    query_size();

    if (tcgetattr(0, &saved_tio) == 0) {
        tio_saved = 1;
        struct termios raw = saved_tio;
        // Character-at-a-time and no echo: a curses program draws the
        // screen itself and must see keys as they are pressed rather
        // than a line at a time.
        raw.c_lflag &= (unsigned)~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }

    // Clear, including scrollback, and home the cursor.
    fputs("\033[H\033[2J\033[3J", stdout);
    fflush(stdout);
    return OK;
}

int endwin(void) {
    fputs("\033[0m\033[?25h\033[H\033[2J", stdout);
    fflush(stdout);
    if (tio_saved) { tcsetattr(0, TCSANOW, &saved_tio); }
    return OK;
}

int refresh(void) { fflush(stdout); return OK; }

int move(int y, int x) {
    printf("\033[%d;%dH", y + 1, x + 1);
    return OK;
}

int printw(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int noecho(void) { return OK; }         // done in initscr

int curs_set(int visibility) {
    fputs(visibility ? "\033[?25h" : "\033[?25l", stdout);
    return OK;
}

int keypad(WINDOW *win, bool enable) {
    (void)win; (void)enable;
    // Escape sequences are always decoded by getch(); there is no mode
    // in which this terminal reports arrow keys any other way.
    return OK;
}

int timeout_shim(int ms) { read_timeout = ms; return OK; }

int start_color(void) { return OK; }
int has_colors(void)  { return TRUE; }

int init_color(short color, short r, short g, short b) {
    if (color < 0 || color >= 256) { return ERR; }
    col_r[color] = r;
    col_g[color] = g;
    col_b[color] = b;
    return OK;
}

// The viewer reads colours back to decide how to shade a material, so
// this must return exactly what init_color stored.
int color_content(short color, short *r, short *g, short *b) {
    if (color < 0 || color >= 256) { return ERR; }
    if (r) { *r = col_r[color]; }
    if (g) { *g = col_g[color]; }
    if (b) { *b = col_b[color]; }
    return OK;
}

// This terminal's palette is writable, which is the whole reason the
// viewer can show a model's real material colours.
int can_change_color(void) { return TRUE; }

int init_pair(short pair, short fg, short bg) {
    (void)bg;                            // the viewer only ever sets 0
    if (pair < 0 || pair >= 256) { return ERR; }
    pair_fg[pair] = fg;
    return OK;
}

int attron(int attrs) {
    int pair = PAIR_NUMBER(attrs);
    if (pair <= 0 || pair >= 256) { return OK; }
    short c = pair_fg[pair];
    if (c < 0 || c >= 256) { return OK; }
    // True colour. term/vt.c understands ESC[38;2;R;G;Bm and maps it
    // onto its 256-colour palette, so the model's material colours
    // arrive as written rather than being quantised here.
    printf("\033[38;2;%d;%d;%dm",
           (col_r[c] * 255) / 1000, (col_g[c] * 255) / 1000, (col_b[c] * 255) / 1000);
    return OK;
}

int attroff(int attrs) {
    (void)attrs;
    fputs("\033[39m", stdout);           // default foreground
    return OK;
}

// One byte, honouring the timeout set by timeout().
static int read_byte(unsigned char *out) {
    if (read_timeout >= 0) {
        struct pollfd pf = { 0, POLLIN, 0 };
        int pr = poll(&pf, 1, read_timeout);
        if (pr <= 0) { return 0; }       // nothing to read
    }
    long n = read(0, out, 1);
    return n == 1 ? 1 : -1;
}

int getch(void) {
    unsigned char c;
    int r = read_byte(&c);
    if (r <= 0) { return ERR; }
    if (c != 0x1B) { return c; }

    // An escape: could be a lone ESC or an arrow key. Both remaining
    // bytes are read with a SHORT timeout rather than the caller's --
    // with timeout(-1) a lone ESC would otherwise block forever waiting
    // for a sequence that is not coming.
    int saved = read_timeout;
    read_timeout = 30;
    unsigned char b1, b2;
    int got1 = read_byte(&b1);
    if (got1 <= 0 || b1 != '[') { read_timeout = saved; return 0x1B; }
    int got2 = read_byte(&b2);
    read_timeout = saved;
    if (got2 <= 0) { return 0x1B; }

    switch (b2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    default:  return 0x1B;
    }
}
