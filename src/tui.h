/* SPDX-License-Identifier: MIT */
/* tui: a small full-screen terminal interface without curses (cleanlinux-setup).
 *
 * The screen is a grid of cells drawn into memory and sent to the terminal line by line, only
 * the lines that changed: cheap on a serial console. Box drawing and other non-ASCII glyphs fall
 * back to ASCII when the console font lacks them (the Linux VT tells its font's Unicode map) or
 * with --ascii. Text is UTF-8; every code point takes one column (Latin, Cyrillic, Greek). */
#ifndef CLI_TUI_H
#define CLI_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* keys: Unicode code points, or these */
enum {
	K_NONE = 0,
	K_UP = 0x110000,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_DEL,
	K_BS,
	K_ENTER,
	K_ESC,
	K_TAB,
	K_BTAB,
	K_CTRL_C,
	K_CTRL_U,
	K_RESIZE,
	K_EOF,
};

/* attributes of cells: the colours of the screens (blue around a light grey panel with a shadow,
 * cyan bars), or plain, bold and reverse without colours */
enum {
	A_NORMAL,    /* text in the panel */
	A_BOLD,      /* emphasis in the panel */
	A_DIM,       /* secondary text in the panel */
	A_TITLE,     /* the bar at the top */
	A_SEL,       /* the selected item, the chosen button */
	A_FIELD,     /* an input field */
	A_FOCUS,     /* the input field being typed into */
	A_WARN,      /* red */
	A_OK,        /* green */
	A_BAR,       /* the filled part of a progress bar */
	A_BG,        /* the screen around the panel */
	A_SHADOW,    /* the shadow of the panel */
	A_LIGHT,     /* the lit edges of the panel's frame */
	A_HEAD,      /* the title of the panel */
	A_HINT,      /* the bar of key hints at the bottom */
	A_KEY,       /* a key in the hints */
	A_STEP,      /* a step to come, in the list of steps */
	A_STEP_DONE, /* a step done */
	A_STEP_CUR,  /* the step of the screen */
	A_BUTTON,    /* a button not chosen */
	A_COUNT
};
extern int tui_rows, tui_cols;
extern bool tui_ascii; /* box drawing in ASCII */
extern bool tui_color;

/* UTF-8 (pure, unit tested) */
uint32_t utf8_next(const char **s); /* the next code point (U+FFFD for a bad byte), advances *s */
size_t utf8_len(const char *s);     /* code points = columns */
int utf8_put(uint32_t cp, char out[4]); /* bytes written */
/* the byte length of the first N code points of S */
size_t utf8_prefix(const char *s, size_t n);
/* TEXT word-wrapped to WIDTH columns: lines separated by '\n' (paragraphs by an empty line) */
char *text_wrap(const char *text, int width);

/* the terminal */
void tui_begin(bool ascii);
void tui_end(void);            /* the terminal as it was (also at exit and on fatal signals) */
void tui_suspend(void);        /* before running a program on the terminal */
void tui_resume(void);
int tui_key(int timeout_ms);   /* a key, K_NONE after the timeout (-1: wait), K_RESIZE */
void tui_reload_font(void);    /* after setfont: the glyphs of the console font again */
void tui_sync_size(void);      /* the size of the terminal again (a new font changes it) */
bool tui_is_vt(void);          /* a Linux virtual terminal */
bool tui_has_glyph(uint32_t cp);

/* drawing into the frame; tui_flush sends what changed */
void tui_clear(void); /* the whole screen in A_BG */
int tui_put(int y, int x, int attr, const char *s, int maxw); /* columns written */
void tui_putc(int y, int x, int attr, uint32_t cp);
void tui_fill(int y, int x, int w, int attr, uint32_t cp);
void tui_box(int y, int x, int h, int w, int attr);
/* a panel: filled with A_NORMAL, a frame lit at the top and left, a shadow right and below */
void tui_panel(int y, int x, int h, int w);
/* TEXT with *keys* in A_KEY and the rest in ATTR; columns written */
int tui_put_marked(int y, int x, int attr, const char *text, int maxw);
void tui_hline(int y, int x, int w, int attr);
void tui_cursor(int y, int x); /* the cursor shown there (y < 0: hidden) */
void tui_flush(void);
void tui_redraw(void);         /* everything again at the next flush */

#endif
