/* SPDX-License-Identifier: MIT */
/* tui: the terminal of cleanlinux-setup (see tui.h) */
#define _GNU_SOURCE
#include "tui.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int tui_rows = 24, tui_cols = 80;
bool tui_ascii, tui_color = true;

/* ---- UTF-8 ---- */

uint32_t utf8_next(const char **sp)
{
	const unsigned char *s = (const unsigned char *)*sp;
	uint32_t cp;
	int n;
	if (s[0] < 0x80) {
		*sp += 1;
		return s[0];
	}
	if ((s[0] & 0xe0) == 0xc0)
		cp = s[0] & 0x1f, n = 1;
	else if ((s[0] & 0xf0) == 0xe0)
		cp = s[0] & 0x0f, n = 2;
	else if ((s[0] & 0xf8) == 0xf0)
		cp = s[0] & 0x07, n = 3;
	else {
		*sp += 1;
		return 0xfffd;
	}
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*sp += i;
			return 0xfffd;
		}
		cp = cp << 6 | (s[i] & 0x3f);
	}
	*sp += n + 1;
	if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
	    (cp >= 0xd800 && cp <= 0xdfff))
		return 0xfffd;
	return cp;
}

size_t utf8_len(const char *s)
{
	size_t n = 0;
	while (*s) {
		utf8_next(&s);
		n++;
	}
	return n;
}

int utf8_put(uint32_t cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

size_t utf8_prefix(const char *s, size_t n)
{
	const char *p = s;
	while (n-- && *p)
		utf8_next(&p);
	return (size_t)(p - s);
}

static size_t span_len(const char *s, const char *end)
{
	size_t n = 0;
	while (s < end && *s) {
		utf8_next(&s);
		n++;
	}
	return n;
}

char *text_wrap(const char *text, int width)
{
	sbuf b = SBUF_INIT;
	size_t w = width < 1 ? 1 : (size_t)width;
	sb_add(&b, "");
	for (const char *p = text; *p;) {
		const char *end = p + strcspn(p, "\n");
		size_t col = 0;
		bool first = true;
		for (const char *q = p; q < end;) {
			/* the spaces before a word stay (columns of a table, an indent), except at a break */
			const char *gap = q;
			while (q < end && *q == ' ')
				q++;
			if (q >= end)
				break;
			size_t gw = (size_t)(q - gap);
			const char *word = q;
			while (q < end && *q != ' ')
				q++;
			size_t wc = span_len(word, q);
			if (!first && col + gw + wc > w) {
				sb_add(&b, "\n");
				col = 0;
				gw = 0;
			}
			if (gw >= w - col)
				gw = 0;
			for (size_t i = 0; i < gw; i++)
				sb_add(&b, " ");
			col += gw;
			first = false;
			/* a word longer than the line is cut */
			while (wc > w - col) {
				size_t take = w - col, bytes = utf8_prefix(word, take);
				sb_addn(&b, word, bytes);
				sb_add(&b, "\n");
				word += bytes;
				wc -= take;
				col = 0;
			}
			sb_addn(&b, word, (size_t)(q - word));
			col += wc;
		}
		p = end;
		if (*p == '\n') {
			sb_add(&b, "\n");
			p++;
		}
	}
	return sb_take(&b);
}

/* ---- the console font ---- */

static uint32_t *glyphs; /* sorted: the code points the VT's font has */
static size_t nglyphs;
static bool have_map;

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

bool tui_is_vt(void)
{
	char type;
	return ioctl(0, KDGKBTYPE, &type) == 0;
}

void tui_reload_font(void)
{
	free(glyphs);
	glyphs = NULL;
	nglyphs = 0;
	have_map = false;
	if (!tui_is_vt())
		return;
	struct unimapdesc ud = {0, NULL};
	if (ioctl(0, GIO_UNIMAP, &ud) < 0 && errno != ENOMEM)
		return;
	if (!ud.entry_ct)
		return;
	unsigned short n = ud.entry_ct;
	ud.entries = xmalloc(n * sizeof(*ud.entries));
	if (ioctl(0, GIO_UNIMAP, &ud) < 0) {
		free(ud.entries);
		return;
	}
	glyphs = xmalloc((ud.entry_ct + 1u) * sizeof(*glyphs));
	for (unsigned i = 0; i < ud.entry_ct; i++)
		glyphs[i] = ud.entries[i].unicode;
	nglyphs = ud.entry_ct;
	qsort(glyphs, nglyphs, sizeof(*glyphs), cmp_u32);
	have_map = true;
	free(ud.entries);
}

bool tui_has_glyph(uint32_t cp)
{
	if (cp < 0x80)
		return true;
	if (!have_map)
		return !tui_ascii;
	return bsearch(&cp, glyphs, nglyphs, sizeof(*glyphs), cmp_u32) != NULL;
}

/* what a glyph becomes when the font or the terminal lacks it; 0: no stand-in */
static uint32_t stand_in(uint32_t cp)
{
	switch (cp) {
	case 0x2500: case 0x2550: return '-';
	case 0x2502: case 0x2551: return '|';
	case 0x250c: case 0x2510: case 0x2514: case 0x2518: case 0x251c: case 0x2524: case 0x252c:
	case 0x2534: case 0x253c: case 0x2554: case 0x2557: case 0x255a: case 0x255d: return '+';
	case 0x2588: case 0x2593: return '#';
	case 0x2591: return '.';
	case 0x2592: return ':';
	case 0x25b6: case 0x25ba: case 0x2192: case 0x203a: return '>';
	case 0x25c0: case 0x25c4: case 0x2190: case 0x2039: return '<';
	case 0x2191: return '^';
	case 0x2193: return 'v';
	case 0x2022: case 0x25cf: return '*';
	case 0x25cb: return 'o';
	case 0x2013: case 0x2014: case 0x2212: return '-';
	case 0x2026: case 0x00b7: return '.';
	case 0x00ab: case 0x00bb: case 0x201c: case 0x201d: case 0x201e: return '"';
	case 0x2018: case 0x2019: return '\'';
	case 0x00d7: return 'x';
	case 0x00a0: return ' ';
	case 0x2713: case 0x2714: return '+';
	default: return 0;
	}
}

/* ---- the terminal ---- */

/* a cell: the code point, the attribute in the top byte (no padding for memcmp) */
typedef uint32_t cell;
#define CELL(cp, attr) ((uint32_t)(cp) | (uint32_t)(attr) << 24)
#define CELL_CP(c) ((c) & 0xffffffu)
#define CELL_ATTR(c) ((int)((c) >> 24))

static struct termios saved;
static bool started, raw_on;
static volatile sig_atomic_t winch;
static cell *cur, *prev;
static int alloc_rows, alloc_cols, cur_y = -1, cur_x;
static bool full;

static const char *const sgr_color[A_COUNT] = {
	[A_NORMAL] = "\033[0m",		[A_BOLD] = "\033[0;1m",		[A_DIM] = "\033[0;2m",
	[A_TITLE] = "\033[0;1;37;44m",	[A_SEL] = "\033[0;30;46m",	[A_FIELD] = "\033[0;30;47m",
	[A_FOCUS] = "\033[0;1;37;44m", [A_WARN] = "\033[0;1;31m",	[A_OK] = "\033[0;1;32m",
	[A_BAR] = "\033[0;1;36m",
};
static const char *const sgr_mono[A_COUNT] = {
	[A_NORMAL] = "\033[0m", [A_BOLD] = "\033[0;1m",	 [A_DIM] = "\033[0m",	 [A_TITLE] = "\033[0;7m",
	[A_SEL] = "\033[0;7m",	[A_FIELD] = "\033[0;4m", [A_FOCUS] = "\033[0;7m", [A_WARN] = "\033[0;1m",
	[A_OK] = "\033[0;1m",	[A_BAR] = "\033[0;1m",
};

static void write_all(const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(1, s, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		s += w;
		n -= (size_t)w;
	}
}

static const char leave_seq[] = "\033[0m\033[2J\033[H\033[?25h\033[?1049l";

static void restore(void)
{
	if (raw_on) {
		tcsetattr(0, TCSAFLUSH, &saved);
		raw_on = false;
	}
}

static void on_fatal(int sig)
{
	if (raw_on) {
		tcsetattr(0, TCSANOW, &saved);
		if (write(1, leave_seq, sizeof(leave_seq) - 1) < 0) {
			/* nothing more to do */
		}
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void on_winch(int sig)
{
	(void)sig;
	winch = 1;
}

static void update_size(void)
{
	struct winsize ws;
	int rows = 0, cols = 0;
	if (ioctl(1, TIOCGWINSZ, &ws) == 0)
		rows = ws.ws_row, cols = ws.ws_col;
	if (rows <= 0 || cols <= 0) {
		/* a serial line does not know: the environment, else the classic size */
		const char *l = getenv("LINES"), *c = getenv("COLUMNS");
		rows = l ? atoi(l) : 0;
		cols = c ? atoi(c) : 0;
	}
	tui_rows = rows > 0 ? rows : 24;
	tui_cols = cols > 0 ? cols : 80;
	if (tui_rows > 500)
		tui_rows = 500;
	if (tui_cols > 500)
		tui_cols = 500;
	if (tui_rows != alloc_rows || tui_cols != alloc_cols) {
		free(cur);
		free(prev);
		size_t n = (size_t)tui_rows * (size_t)tui_cols;
		cur = xmalloc(n * sizeof(*cur));
		prev = xmalloc(n * sizeof(*prev));
		alloc_rows = tui_rows, alloc_cols = tui_cols;
	}
	full = true;
	tui_clear();
}

static void raw_mode(void)
{
	struct termios t = saved;
	t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
	t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR | ISTRIP | BRKINT);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &t);
	raw_on = true;
	static const char enter[] = "\033[?1049h\033[0m\033[2J\033[?25l";
	write_all(enter, sizeof(enter) - 1);
	full = true;
}

void tui_begin(bool ascii)
{
	const char *term = getenv("TERM");
	tui_color = !(term && !strcmp(term, "dumb"));
	/* serial terminals (agetty's vt220) are not trusted with UTF-8 box drawing; the VT says
	 * what its font has */
	tui_ascii = ascii || !term || !strcmp(term, "dumb") || !strncmp(term, "vt1", 3) || !strncmp(term, "vt2", 3);
	if (tui_is_vt()) {
		tui_ascii = ascii;
		tui_reload_font();
	}
	if (tcgetattr(0, &saved) < 0)
		die("cleanlinux-setup needs a terminal");
	struct sigaction sa = {0};
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = on_fatal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	started = true;
	atexit(tui_end);
	raw_mode();
	update_size();
}

void tui_end(void)
{
	if (!started)
		return;
	if (raw_on)
		write_all(leave_seq, sizeof(leave_seq) - 1);
	restore();
	started = false;
}

void tui_suspend(void)
{
	if (raw_on)
		write_all(leave_seq, sizeof(leave_seq) - 1);
	restore();
}

void tui_resume(void)
{
	raw_mode();
	update_size();
}

void tui_redraw(void) { full = true; }

void tui_sync_size(void) { update_size(); }

/* ---- keys ---- */

static unsigned char kbuf[64];
static size_t klen;

static int take(size_t n, int key)
{
	memmove(kbuf, kbuf + n, klen - n);
	klen -= n;
	return key;
}

/* the key at the start of kbuf; K_NONE with nothing taken when it is incomplete (FORCE: take
 * what is there) */
static int parse_key(bool force, bool *took)
{
	unsigned char c = kbuf[0];
	*took = true;
	if (c == 0x1b) {
		if (klen == 1) {
			if (force)
				return take(1, K_ESC);
			*took = false;
			return K_NONE;
		}
		if (kbuf[1] == '[' || kbuf[1] == 'O') {
			/* the Linux console's F1-F5: ESC [ [ A..E */
			if (kbuf[1] == '[' && klen > 2 && kbuf[2] == '[')
				return klen > 3 || force ? take(klen > 3 ? 4 : klen, K_NONE) : (*took = false, K_NONE);
			size_t i = 2;
			while (i < klen && kbuf[i] >= 0x30 && kbuf[i] <= 0x3f)
				i++;
			if (i >= klen) {
				if (force)
					return take(klen, K_NONE);
				*took = false;
				return K_NONE;
			}
			int key = K_NONE, num = atoi((const char *)kbuf + 2);
			switch (kbuf[i]) {
			case 'A': key = K_UP; break;
			case 'B': key = K_DOWN; break;
			case 'C': key = K_RIGHT; break;
			case 'D': key = K_LEFT; break;
			case 'H': key = K_HOME; break;
			case 'F': key = K_END; break;
			case 'Z': key = K_BTAB; break;
			case 'M': key = K_ENTER; break;
			case '~':
				key = num == 1 || num == 7 ? K_HOME
				      : num == 4 || num == 8 ? K_END
				      : num == 3 ? K_DEL
				      : num == 5 ? K_PGUP
				      : num == 6 ? K_PGDN
						 : K_NONE;
				break;
			default: break;
			}
			return take(i + 1, key);
		}
		/* Alt+key: Esc, the key comes next */
		return take(1, K_ESC);
	}
	if (c == '\r' || c == '\n')
		return take(c == '\r' && klen > 1 && kbuf[1] == '\n' ? 2 : 1, K_ENTER);
	if (c == 0x7f || c == 0x08)
		return take(1, K_BS);
	if (c == '\t')
		return take(1, K_TAB);
	if (c == 0x03)
		return take(1, K_CTRL_C);
	if (c == 0x15)
		return take(1, K_CTRL_U);
	if (c < 0x20)
		return take(1, K_NONE);
	size_t need = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
	if (klen < need) {
		if (force)
			return take(klen, K_NONE);
		*took = false;
		return K_NONE;
	}
	char tmp[5] = {0};
	memcpy(tmp, kbuf, need);
	const char *p = tmp;
	uint32_t cp = utf8_next(&p);
	return take(need, (int)cp);
}

int tui_key(int timeout_ms)
{
	for (;;) {
		if (winch) {
			winch = 0;
			update_size();
			return K_RESIZE;
		}
		while (klen) {
			bool took;
			int k = parse_key(false, &took);
			if (k != K_NONE)
				return k;
			if (!took)
				break;
		}
		struct pollfd p = {0, POLLIN, 0};
		int r = poll(&p, 1, klen ? 60 : timeout_ms);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return K_EOF;
		}
		if (r == 0) {
			if (!klen)
				return K_NONE;
			bool took;
			int k = parse_key(true, &took);
			if (k != K_NONE)
				return k;
			continue;
		}
		ssize_t n = read(0, kbuf + klen, sizeof(kbuf) - klen);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return K_EOF;
		}
		if (n == 0)
			return K_EOF;
		klen += (size_t)n;
	}
}

/* ---- drawing ---- */

void tui_clear(void)
{
	size_t n = (size_t)tui_rows * (size_t)tui_cols;
	for (size_t i = 0; i < n; i++)
		cur[i] = CELL(' ', A_NORMAL);
	cur_y = -1;
}

void tui_putc(int y, int x, int attr, uint32_t cp)
{
	if (y < 0 || x < 0 || y >= tui_rows || x >= tui_cols)
		return;
	if (cp < 0x20 || cp == 0x7f)
		cp = '?';
	cur[(size_t)y * (size_t)tui_cols + (size_t)x] = CELL(cp, attr);
}

int tui_put(int y, int x, int attr, const char *s, int maxw)
{
	int n = 0;
	while (*s && (maxw < 0 || n < maxw)) {
		uint32_t cp = utf8_next(&s);
		tui_putc(y, x + n, attr, cp);
		n++;
	}
	return n;
}

void tui_fill(int y, int x, int w, int attr, uint32_t cp)
{
	for (int i = 0; i < w; i++)
		tui_putc(y, x + i, attr, cp);
}

void tui_hline(int y, int x, int w, int attr)
{
	tui_fill(y, x, w, attr, 0x2500);
}

void tui_box(int y, int x, int h, int w, int attr)
{
	if (h < 2 || w < 2)
		return;
	tui_putc(y, x, attr, 0x250c);
	tui_putc(y, x + w - 1, attr, 0x2510);
	tui_putc(y + h - 1, x, attr, 0x2514);
	tui_putc(y + h - 1, x + w - 1, attr, 0x2518);
	tui_fill(y, x + 1, w - 2, attr, 0x2500);
	tui_fill(y + h - 1, x + 1, w - 2, attr, 0x2500);
	for (int i = 1; i < h - 1; i++) {
		tui_putc(y + i, x, attr, 0x2502);
		tui_putc(y + i, x + w - 1, attr, 0x2502);
	}
}

void tui_cursor(int y, int x)
{
	cur_y = y;
	cur_x = x;
}

static void emit_cp(sbuf *b, uint32_t cp)
{
	if (cp >= 0x80) {
		uint32_t s = stand_in(cp);
		if (s && (tui_ascii || !tui_has_glyph(cp)))
			cp = s;
	}
	char u[4];
	sb_addn(b, u, (size_t)utf8_put(cp, u));
}

void tui_flush(void)
{
	sbuf b = SBUF_INIT;
	const char *const *sgr = tui_color ? sgr_color : sgr_mono;
	sb_add(&b, "\033[?25l");
	for (int y = 0; y < tui_rows; y++) {
		cell *row = cur + (size_t)y * (size_t)tui_cols, *old = prev + (size_t)y * (size_t)tui_cols;
		if (!full && !memcmp(row, old, (size_t)tui_cols * sizeof(*row)))
			continue;
		sb_printf(&b, "\033[%d;1H", y + 1);
		int attr = -1;
		/* not the last cell of the screen: some terminals scroll after it */
		int w = y == tui_rows - 1 ? tui_cols - 1 : tui_cols;
		for (int x = 0; x < w; x++) {
			if (CELL_ATTR(row[x]) != attr) {
				attr = CELL_ATTR(row[x]);
				sb_add(&b, sgr[attr < A_COUNT ? attr : A_NORMAL]);
			}
			emit_cp(&b, CELL_CP(row[x]));
		}
		sb_add(&b, "\033[0m");
	}
	if (cur_y >= 0)
		sb_printf(&b, "\033[%d;%dH\033[?25h", cur_y + 1, cur_x + 1);
	char *s = sb_take(&b);
	write_all(s, strlen(s));
	free(s);
	memcpy(prev, cur, (size_t)tui_rows * (size_t)tui_cols * sizeof(*cur));
	full = false;
}
