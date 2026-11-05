/* SPDX-License-Identifier: MIT */
/* cleanlinux-setup: the installer of CleanLinux with screens, for the live system of the ISO.
 *
 *   cleanlinux-setup [--lang en|ru] [--ascii] [--dry-run]
 *   cleanlinux-setup --auto        what the live system starts at login: on its main console
 *                                  only, and not again once quit
 *
 * A few screens on the console (a Linux VT or a serial line; 80x24 is enough): the language, the
 * keyboard layout (loaded at once), the time zone, the disk, encryption, the computer's name, the
 * user and the passwords, the hardware profile, a summary. Esc goes back, Ctrl+C quits, nothing
 * changes before the last confirmation. Then cleanlinux-install does the work with these answers:
 * the passwords in files only root can read (a 0700 directory in /run, wiped afterwards), its
 * output in /tmp/cleanlinux-install.log, its progress on the screen. The console= and modeset
 * arguments of the running kernel go to the new system's command line (a serial console, or
 * nomodeset from the safe graphics entry of the ISO). */
#define _GNU_SOURCE
#include "install.h"
#include "setup_text.h"
#include "tui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE "/tmp/cleanlinux-install.log"
#define LOCK_FILE "/run/cleanlinux-setup.lock"
#define EXIT_MARK "/run/cleanlinux-setup.exit"
#define LIVE_DIR "/run/cleanlinux/live"
#define ESP_MIB 512
#define MIN_BYTES (((uint64_t)ESP_MIB << 20) + (3ULL << 30))
#define STEPS 10

enum { BACK = 0, NEXT = 1, QUIT = -2 };
#define R_BACK (-1)
#define R_QUIT (-2)

static int lang; /* 0 English, 1 Russian */
#define T(id) (strings[id][lang])

static bool dry_run, big_screen;
static int step_no;
/* where kbd's keymaps and fonts and tzdata's zone.tab are: / (CLEANLINUX_SETUP_DATA, for the tests) */
static const char *data_root = "/";

static char *tf(int id, ...)
{
	va_list ap;
	va_start(ap, id);
	char *s = NULL;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
	if (vasprintf(&s, T(id), ap) < 0)
		die("out of memory");
#pragma GCC diagnostic pop
	va_end(ap);
	return s;
}

/* ---- keymaps and fonts ---- */

/* font: "" the kernel's, "cyr" the first Cyrillic font kbd has, else a font of kbd */
static const struct kmap {
	const char *map, *en, *ru, *font;
} kmaps[] = {
	{"us", "English (US)", "Английская (США)", ""},
	{"ruwin_alt_sh-UTF-8", "Russian", "Русская", "cyr"},
	{"ua-utf", "Ukrainian", "Украинская", "cyr"},
	{"by", "Belarusian", "Белорусская", "cyr"},
	{"kazakh", "Kazakh", "Казахская", "cyr"},
	{"uk", "English (UK)", "Английская (Великобритания)", ""},
	{"dvorak", "English (Dvorak)", "Английская (Дворак)", ""},
	{"de-latin1", "German", "Немецкая", "lat9w-16"},
	{"fr-latin9", "French", "Французская", "lat9w-16"},
	{"es", "Spanish", "Испанская", "lat9w-16"},
	{"it", "Italian", "Итальянская", "lat9w-16"},
	{"pt-latin1", "Portuguese", "Португальская", "lat9w-16"},
	{"br-abnt2", "Portuguese (Brazil)", "Португальская (Бразилия)", "lat9w-16"},
	{"pl2", "Polish", "Польская", "lat2-16"},
	{"cz", "Czech", "Чешская", "lat2-16"},
	{"hu", "Hungarian", "Венгерская", "lat2-16"},
	{"trq", "Turkish", "Турецкая", "iso09.16"},
	{"sv-latin1", "Swedish", "Шведская", "lat9w-16"},
	{"fi", "Finnish", "Финская", "lat9w-16"},
	{"no", "Norwegian", "Норвежская", "lat9w-16"},
	{"dk", "Danish", "Датская", "lat9w-16"},
	{"gr", "Greek", "Греческая", "iso07u-16"},
	{"jp106", "Japanese (106 keys)", "Японская (106 клавиш)", ""},
};
static const char *const cyr_fonts[] = {"latarcyrheb-sun16", "cyr-sun16", "UniCyr_8x16"};

static bool have_font(const char *name)
{
	char *f = kbd_file(data_root, "consolefonts", name);
	bool ok = f != NULL;
	free(f);
	return ok;
}

/* the console font for WANT (see kmaps), or NULL for the kernel's */
static const char *font_for(const char *want)
{
	if (big_screen && have_font("latarcyrheb-sun32"))
		return "latarcyrheb-sun32";
	if (!want || !*want)
		return NULL;
	if (!strcmp(want, "cyr")) {
		for (size_t i = 0; i < ARRAY_LEN(cyr_fonts); i++)
			if (have_font(cyr_fonts[i]))
				return cyr_fonts[i];
		return NULL;
	}
	return have_font(want) ? want : NULL;
}

/* a program with the terminal as its input (loadkeys and setfont find the console there), its
 * output thrown away */
static int quiet(const char *name, ...)
{
	strv a = STRV_INIT;
	strv_push(&a, name);
	va_list ap;
	va_start(ap, name);
	const char *s;
	while ((s = va_arg(ap, const char *)))
		strv_push(&a, s);
	va_end(ap);
	pid_t pid = fork();
	if (pid < 0) {
		strv_free(&a);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, 1);
			dup2(null, 2);
		}
		execvp(name, strv_argv(&a));
		_exit(127);
	}
	strv_free(&a);
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static const char *live_font;

static void set_font(const char *font)
{
	if (!font || !tui_is_vt() || (live_font && !strcmp(live_font, font)))
		return;
	if (quiet("setfont", font, (char *)NULL) == 0) {
		live_font = font;
		tui_reload_font();
		tui_sync_size();
	}
}

/* ---- disks ---- */

struct dinfo {
	char *name, *dev, *model, *tran, *content, *size;
	uint64_t bytes;
	int why; /* 0: can be chosen; S_DISK_MEDIUM, S_DISK_BUSY, S_DISK_SMALL */
};
static struct dinfo *disks;
static size_t ndisks;

static void disks_free(void)
{
	for (size_t i = 0; i < ndisks; i++) {
		free(disks[i].name);
		free(disks[i].dev);
		free(disks[i].model);
		free(disks[i].tran);
		free(disks[i].content);
		free(disks[i].size);
	}
	free(disks);
	disks = NULL;
	ndisks = 0;
}

static bool skip_disk(const char *n)
{
	static const char *const pre[] = {"loop", "ram", "zram", "sr", "fd", "md", "dm-", "nbd", "mtdblock", "zd", "ubi"};
	for (size_t i = 0; i < ARRAY_LEN(pre); i++)
		if (str_prefix(n, pre[i]))
			return true;
	return (str_prefix(n, "mmcblk") && strstr(n, "boot")) || strstr(n, "rpmb");
}

/* the disk (a name in /sys/block) of the block device MAJ:MIN */
static char *disk_of(unsigned maj, unsigned min)
{
	char *l = xasprintf("/sys/dev/block/%u:%u", maj, min);
	char *p = realpath(l, NULL);
	free(l);
	if (!p)
		return NULL;
	char *part = xasprintf("%s/partition", p);
	if (exists(part))
		*strrchr(p, '/') = '\0';
	free(part);
	char *name = xstrdup(strrchr(p, '/') + 1);
	free(p);
	return name;
}

static void add_devpath(strv *out, const char *path)
{
	struct stat st;
	if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode))
		return;
	char *d = disk_of(major(st.st_rdev), minor(st.st_rdev));
	if (d && !strv_contains(out, d))
		strv_take(out, d);
	else
		free(d);
}

static bool dir_has_entries(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return false;
	struct dirent *e;
	bool any = false;
	while (!any && (e = readdir(d)))
		any = e->d_name[0] != '.';
	closedir(d);
	return any;
}

/* the disks in use: something mounted from them, swap, a device-mapper (LUKS, LVM) on top; the
 * medium of the live system apart */
static void scan_busy(strv *busy, strv *medium)
{
	char *mi = read_file("/proc/self/mountinfo", NULL);
	for (char *save = NULL, *l = mi ? strtok_r(mi, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		unsigned maj, min;
		char dir[4096];
		if (sscanf(l, "%*u %*u %u:%u %*s %4095s", &maj, &min, dir) != 3)
			continue;
		char *d = disk_of(maj, min);
		if (!d)
			continue;
		char *real = mountinfo_unescape(dir);
		if (str_prefix(real, LIVE_DIR "/") && !strv_contains(medium, d))
			strv_push(medium, d);
		free(real);
		if (!strv_contains(busy, d))
			strv_take(busy, d);
		else
			free(d);
	}
	free(mi);
	char *sw = read_file("/proc/swaps", NULL);
	char *save = NULL, *l = sw ? strtok_r(sw, "\n", &save) : NULL; /* the header */
	while (l && (l = strtok_r(NULL, "\n", &save))) {
		l[strcspn(l, " \t")] = '\0';
		add_devpath(busy, l);
	}
	free(sw);
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char *h = xasprintf("/sys/block/%s/holders", e->d_name);
		bool held = dir_has_entries(h);
		free(h);
		DIR *pd = held ? NULL : (h = xasprintf("/sys/block/%s", e->d_name), opendir(h));
		if (!held)
			free(h);
		struct dirent *pe;
		while (pd && !held && (pe = readdir(pd))) {
			if (!str_prefix(pe->d_name, e->d_name) || !strcmp(pe->d_name, e->d_name))
				continue;
			char *ph = xasprintf("/sys/block/%s/%s/holders", e->d_name, pe->d_name);
			held = dir_has_entries(ph);
			free(ph);
		}
		if (pd)
			closedir(pd);
		if (held && !strv_contains(busy, e->d_name))
			strv_push(busy, e->d_name);
	}
	if (d)
		closedir(d);
}

static char *sys_line(const char *sys, const char *file)
{
	char *p = xasprintf("%s/%s", sys, file);
	char *v = read_line(p);
	free(p);
	if (v && !*v) {
		free(v);
		v = NULL;
	}
	return v;
}

static char *disk_model(const char *sys)
{
	char *v = sys_line(sys, "device/vendor"), *m = sys_line(sys, "device/model");
	if (!m)
		m = sys_line(sys, "device/name"); /* SD cards and eMMC */
	if (v && !strncmp(v, "0x", 2)) {
		/* a number, not a name: the PCI vendor of a virtio disk (0x1af4) */
		free(v);
		v = NULL;
	}
	char *out;
	if (v && m && strcmp(v, "ATA") && strncasecmp(m, v, strlen(v)))
		out = xasprintf("%s %s", v, m);
	else
		out = xstrdup(m ? m : v ? v : "");
	free(v);
	free(m);
	return out;
}

static char *disk_tran(const char *name, const char *sys)
{
	char *real = realpath(sys, NULL);
	const char *t = str_prefix(name, "nvme")	   ? "NVMe"
			: str_prefix(name, "mmcblk")	   ? "SD/eMMC"
			: str_prefix(name, "vd")	   ? "virtio"
			: real && strstr(real, "/usb")	   ? "USB"
			: real && strstr(real, "/ata")	   ? "SATA"
			: str_prefix(name, "sd")	   ? "SCSI"
							   : "";
	free(real);
	return xstrdup(t);
}

/* the filesystems on a disk now: "ntfs, vfat"; NULL when nothing is recognised */
static char *disk_content(const char *name)
{
	strv types = STRV_INIT;
	char *sys = xasprintf("/sys/block/%s", name);
	DIR *d = opendir(sys);
	struct dirent *e;
	bool parts = false;
	while (d && (e = readdir(d))) {
		if (!str_prefix(e->d_name, name) || !strcmp(e->d_name, name))
			continue;
		char *pf = xasprintf("%s/%s/partition", sys, e->d_name);
		bool is_part = exists(pf);
		free(pf);
		if (!is_part)
			continue;
		parts = true;
		char *dev = xasprintf("/dev/%s", e->d_name);
		char *t = fstype_of(dev);
		if (t && !strv_contains(&types, t))
			strv_take(&types, t);
		else
			free(t);
		free(dev);
	}
	if (d)
		closedir(d);
	free(sys);
	if (!parts) {
		char *dev = xasprintf("/dev/%s", name), *t = fstype_of(dev);
		if (t)
			strv_take(&types, t);
		free(dev);
	}
	if (!types.n) {
		strv_free(&types);
		return NULL;
	}
	sbuf b = SBUF_INIT;
	for (size_t i = 0; i < types.n; i++)
		sb_printf(&b, "%s%s", i ? ", " : "", types.v[i]);
	strv_free(&types);
	return sb_take(&b);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add_disk(const char *name, uint64_t bytes, char *model, char *tran, char *content, int why)
{
	disks = xrealloc(disks, (ndisks + 1) * sizeof(*disks));
	struct dinfo *x = &disks[ndisks++];
	x->name = xstrdup(name);
	x->dev = xasprintf("/dev/%s", name);
	x->model = model;
	x->tran = tran;
	x->content = content;
	x->bytes = bytes;
	x->size = size_text(bytes);
	x->why = why ? why : bytes < MIN_BYTES ? S_DISK_SMALL : 0;
}

/* CLEANLINUX_SETUP_DISKS="vdb=8192=QEMU disk=ntfs;sdz=1024=Tiny;sdy=16384=Stick==medium": disks
 * that are not there, for the tests of the screens. NAME=MiB=MODEL=FILESYSTEMS=busy|medium */
static void fake_disks(const char *spec)
{
	char *s = xstrdup(spec);
	for (char *save = NULL, *d = strtok_r(s, ";", &save); d; d = strtok_r(NULL, ";", &save)) {
		char *f[5] = {d, NULL, NULL, NULL, NULL};
		for (int i = 1; i < 5 && f[i - 1]; i++) {
			char *eq = strchr(f[i - 1], '=');
			if (eq) {
				*eq = '\0';
				f[i] = eq + 1;
			}
		}
		uint64_t mib = f[1] ? strtoull(f[1], NULL, 10) : 8192;
		int why = !f[4] ? 0 : !strcmp(f[4], "busy") ? S_DISK_BUSY : !strcmp(f[4], "medium") ? S_DISK_MEDIUM : 0;
		add_disk(f[0], mib << 20, xstrdup(f[2] ? f[2] : ""), xstrdup("test"), f[3] && *f[3] ? xstrdup(f[3]) : NULL,
			 why);
	}
	free(s);
}

static void load_disks(void)
{
	disks_free();
	const char *fake = getenv("CLEANLINUX_SETUP_DISKS");
	if (fake) {
		fake_disks(fake);
		return;
	}
	strv names = STRV_INIT, busy = STRV_INIT, medium = STRV_INIT;
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (e->d_name[0] != '.' && !skip_disk(e->d_name))
			strv_push(&names, e->d_name);
	if (d)
		closedir(d);
	if (names.n)
		qsort(names.v, names.n, sizeof(char *), cmp_str);
	scan_busy(&busy, &medium);
	for (size_t i = 0; i < names.n; i++) {
		char *sys = xasprintf("/sys/block/%s", names.v[i]);
		uint64_t sectors = sysfs_u64(sys, "size");
		if (sectors) {
			int why = strv_contains(&medium, names.v[i]) ? S_DISK_MEDIUM
				  : strv_contains(&busy, names.v[i]) ? S_DISK_BUSY
								      : 0;
			add_disk(names.v[i], sectors * 512, disk_model(sys), disk_tran(names.v[i], sys),
				 disk_content(names.v[i]), why);
		}
		free(sys);
	}
	strv_free(&names);
	strv_free(&busy);
	strv_free(&medium);
}

/* ---- time zones (tzdata: zone.tab) ---- */

static strv zones;

static void load_zones(void)
{
	char *zt = xasprintf("%s/usr/share/zoneinfo/zone.tab", strcmp(data_root, "/") ? data_root : "");
	char *t = read_file(zt, NULL);
	free(zt);
	for (char *save = NULL, *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		if (*l == '#')
			continue;
		char *f = strchr(l, '\t');
		f = f ? strchr(f + 1, '\t') : NULL;
		if (!f)
			continue;
		f++;
		f[strcspn(f, "\t")] = '\0';
		if (valid_zone(f) && strchr(f, '/') && !strv_contains(&zones, f))
			strv_push(&zones, f);
	}
	free(t);
	if (zones.n)
		qsort(zones.v, zones.n, sizeof(char *), cmp_str);
}

static const struct {
	const char *name;
	int label;
} regions[] = {
	{"Africa", S_REGION_AFRICA},	 {"America", S_REGION_AMERICA},	    {"Antarctica", S_REGION_ANTARCTICA},
	{"Arctic", S_REGION_ARCTIC},	 {"Asia", S_REGION_ASIA},	    {"Atlantic", S_REGION_ATLANTIC},
	{"Australia", S_REGION_AUSTRALIA}, {"Europe", S_REGION_EUROPE},	    {"Indian", S_REGION_INDIAN},
	{"Pacific", S_REGION_PACIFIC},
};

/* ---- hardware profiles (burn) ---- */

static char *prof_cur, *prof_dev, *prof_model;

static void load_profiles(void)
{
	char *conf = read_file("/etc/burn/burn.conf", NULL);
	prof_cur = conf ? conf_get(conf, "PROFILE") : NULL;
	free(conf);
	if (!prof_cur || !*prof_cur) {
		free(prof_cur);
		prof_cur = xstrdup("native");
	}
	char *const argv[] = {"burn", "profile", "detect", NULL};
	char *out = run_output(argv);
	char *det = detected_profile(out);
	if (det && strcmp(det, "native") && strcmp(det, prof_cur)) {
		prof_dev = det;
		det = NULL;
		for (char *save = NULL, *l = out ? strtok_r(out, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save))
			if (str_prefix(l, "model ")) {
				free(prof_model);
				prof_model = xstrdup(trim(l + 6));
			}
	}
	free(det);
	free(out);
}

/* ---- the screens' parts ---- */

struct area {
	int y, x, h, w;
};

static struct area frame(const char *title, const char *hint)
{
	tui_clear();
	tui_fill(0, 0, tui_cols, A_TITLE, ' ');
	char *head = dry_run ? xasprintf(" %s (%s)", T(S_TITLE), T(S_DRY_TAG)) : xasprintf(" %s", T(S_TITLE));
	tui_put(0, 0, A_TITLE, head, tui_cols);
	free(head);
	if (step_no > 0) {
		char *s = tf(S_STEP, step_no, STEPS);
		int n = (int)utf8_len(s);
		if (n + 2 < tui_cols)
			tui_put(0, tui_cols - n - 1, A_TITLE, s, n);
		free(s);
	}
	int w = tui_cols - 2 > 78 ? 78 : tui_cols - 2, h = tui_rows - 3 > 24 ? 24 : tui_rows - 3;
	int y = 1 + (tui_rows - 2 - h) / 2, x = (tui_cols - w) / 2;
	tui_box(y, x, h, w, A_NORMAL);
	char *t = xasprintf(" %s ", title);
	tui_put(y, x + 2, A_BOLD, t, w - 4);
	free(t);
	tui_put(tui_rows - 1, 1, A_DIM, hint, tui_cols - 2);
	return (struct area){y + 2, x + 2, h - 3, w - 4};
}

/* TEXT wrapped from row Y; the row after it */
static int draw_text(const struct area *a, int y, int attr, const char *text)
{
	char *w = text_wrap(text, a->w);
	for (char *p = w;;) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		if (y < a->y + a->h)
			tui_put(y, a->x, attr, p, a->w);
		y++;
		if (!nl)
			break;
		p = nl + 1;
	}
	free(w);
	return y;
}

static bool want_quit(void);
static bool asking_quit, installing;

/* buttons: the index chosen, -1 for Esc, -2 to quit (Ctrl+C confirmed, or the terminal is gone) */
static int dialog(const char *title, const char *text, const char *warn, const char *const *buttons, int nb, int sel)
{
	for (;;) {
		struct area a = frame(title, T(S_HINT_BUTTONS));
		int y = draw_text(&a, a.y, A_NORMAL, text);
		if (warn)
			draw_text(&a, y + 1, A_WARN, warn);
		int bx = a.x, by = a.y + a.h - 1;
		for (int i = 0; i < nb; i++) {
			char *b = xasprintf("[ %s ]", buttons[i]);
			bx += tui_put(by, bx, i == sel ? A_SEL : A_BOLD, b, -1) + 2;
			free(b);
		}
		tui_flush();
		switch (tui_key(-1)) {
		case K_LEFT:
		case K_UP:
		case K_BTAB: sel = (sel + nb - 1) % nb; break;
		case K_RIGHT:
		case K_DOWN:
		case K_TAB: sel = (sel + 1) % nb; break;
		case K_ENTER: return sel;
		case K_ESC: return -1;
		case K_CTRL_C:
			/* the question itself, or after the installation started: as Esc */
			if (asking_quit || installing)
				return -1;
			if (want_quit())
				return -2;
			break;
		case K_EOF: return -2;
		default: break;
		}
	}
}

static bool want_quit(void)
{
	const char *b[] = {T(S_CONTINUE), T(S_QUIT)};
	int saved = step_no;
	step_no = 0;
	asking_quit = true;
	int r = dialog(T(S_QUIT_TITLE), T(S_QUIT_TEXT), NULL, b, 2, 0);
	asking_quit = false;
	step_no = saved;
	return r == 1 || r == -2;
}

struct item {
	char *label, *detail, *note;
	bool off;
};

static void items_free(struct item *it, int n)
{
	for (int i = 0; i < n; i++) {
		free(it[i].label);
		free(it[i].detail);
		free(it[i].note);
	}
	free(it);
}

static uint32_t fold(uint32_t c)
{
	if (c >= 'A' && c <= 'Z')
		return c + 32;
	if (c >= 0x410 && c <= 0x42f)
		return c + 0x20;
	if (c == 0x401)
		return 0x451;
	return c;
}

static bool match_at(const char *s, const char *f)
{
	while (*f) {
		if (!*s || fold(utf8_next(&s)) != fold(utf8_next(&f)))
			return false;
	}
	return true;
}

static bool matches(const char *s, const char *f)
{
	if (!*f)
		return true;
	for (const char *p = s; *p; utf8_next(&p))
		if (match_at(p, f))
			return true;
	return false;
}

/* a list: the index chosen, R_BACK, R_QUIT; SEARCH filters it by what is typed */
static int list_screen(const char *title, const char *text, const char *warn, const struct item *items, int n, int sel,
		       bool search)
{
	char filter[64] = "";
	int *vis = xmalloc(((size_t)n + 1) * sizeof(int));
	int top = 0, result;
	for (;;) {
		int nv = 0, vsel = -1;
		for (int i = 0; i < n; i++)
			if (matches(items[i].label, filter) || (items[i].detail && matches(items[i].detail, filter))) {
				if (i == sel)
					vsel = nv;
				vis[nv++] = i;
			}
		if (vsel < 0 && nv) {
			vsel = 0;
			sel = vis[0];
		}
		struct area a = frame(title, T(search ? S_HINT_SEARCH : S_HINT_LIST));
		int y = a.y;
		if (text)
			y = draw_text(&a, y, A_NORMAL, text) + 1;
		if (warn)
			y = draw_text(&a, y, A_WARN, warn) + 1;
		if (search && *filter) {
			int w = tui_put(y, a.x, A_BOLD, T(S_SEARCH), a.w);
			tui_put(y, a.x + w + 1, A_FOCUS, filter, a.w - w - 1);
			y += 2;
		}
		int rows = a.y + a.h - 2 - y;
		if (rows < 1)
			rows = 1;
		if (vsel >= 0 && vsel < top)
			top = vsel;
		if (vsel >= top + rows)
			top = vsel - rows + 1;
		if (top > nv - rows)
			top = nv - rows > 0 ? nv - rows : 0;
		for (int r = 0; r < rows && top + r < nv; r++) {
			const struct item *it = &items[vis[top + r]];
			bool on = top + r == vsel;
			int attr = on ? A_SEL : it->off ? A_DIM : A_NORMAL;
			tui_fill(y + r, a.x, a.w, attr, ' ');
			if (on)
				tui_putc(y + r, a.x, attr, 0x25b6);
			int dw = it->detail ? (int)utf8_len(it->detail) : 0;
			if (dw > a.w / 2)
				dw = a.w / 2;
			tui_put(y + r, a.x + 2, attr, it->label, a.w - 3 - (dw ? dw + 2 : 0));
			if (dw)
				tui_put(y + r, a.x + a.w - dw - 1, attr, it->detail, dw);
		}
		if (top > 0)
			tui_putc(y, a.x + a.w, A_DIM, 0x2191);
		if (top + rows < nv)
			tui_putc(y + rows - 1, a.x + a.w, A_DIM, 0x2193);
		if (vsel >= 0 && items[sel].note)
			tui_put(a.y + a.h - 1, a.x, items[sel].off ? A_WARN : A_DIM, items[sel].note, a.w);
		tui_flush();
		int k = tui_key(-1);
		switch (k) {
		case K_UP:
			if (vsel > 0)
				sel = vis[vsel - 1];
			break;
		case K_DOWN:
			if (vsel + 1 < nv)
				sel = vis[vsel + 1];
			break;
		case K_PGUP:
			if (nv)
				sel = vis[vsel - rows > 0 ? vsel - rows : 0];
			break;
		case K_PGDN:
			if (nv)
				sel = vis[vsel + rows < nv - 1 ? vsel + rows : nv - 1];
			break;
		case K_HOME:
			if (nv)
				sel = vis[0];
			break;
		case K_END:
			if (nv)
				sel = vis[nv - 1];
			break;
		case K_ENTER:
			if (vsel >= 0 && !items[sel].off) {
				result = sel;
				goto out;
			}
			break;
		case K_ESC:
			if (*filter) {
				*filter = '\0';
				break;
			}
			result = R_BACK;
			goto out;
		case K_BS:
			if (*filter) {
				size_t n_cp = utf8_len(filter);
				filter[utf8_prefix(filter, n_cp - 1)] = '\0';
			}
			break;
		case K_CTRL_U: *filter = '\0'; break;
		case K_CTRL_C:
			if (want_quit()) {
				result = R_QUIT;
				goto out;
			}
			break;
		case K_EOF: result = R_QUIT; goto out;
		default:
			if (search && k >= 0x20 && k < 0x110000) {
				char u[4];
				int ul = utf8_put((uint32_t)k, u);
				size_t fl = strlen(filter);
				if (fl + (size_t)ul < sizeof(filter)) {
					memcpy(filter + fl, u, (size_t)ul);
					filter[fl + (size_t)ul] = '\0';
				}
			}
			break;
		}
	}
out:
	free(vis);
	return result;
}

struct field {
	const char *label;
	char *buf;
	size_t cap, pos; /* pos: the cursor, in code points */
	bool hidden;
};

typedef const char *(*check_fn)(struct field *f, int *focus);

static void buf_insert(struct field *f, uint32_t cp)
{
	char u[4];
	size_t n = (size_t)utf8_put(cp, u), len = strlen(f->buf), at = utf8_prefix(f->buf, f->pos);
	if (len + n + 1 > f->cap)
		return;
	memmove(f->buf + at + n, f->buf + at, len - at + 1);
	memcpy(f->buf + at, u, n);
	f->pos++;
}

static void buf_delete(struct field *f, size_t pos)
{
	size_t at = utf8_prefix(f->buf, pos), next = utf8_prefix(f->buf, pos + 1);
	memmove(f->buf + at, f->buf + next, strlen(f->buf + next) + 1);
}

/* fields to type into: NEXT, BACK, QUIT; CHECK says what is wrong (and where) or NULL */
static int form_screen(const char *title, const char *text, struct field *f, int n, check_fn check)
{
	int focus = 0;
	const char *err = NULL;
	for (int i = 0; i < n; i++)
		f[i].pos = utf8_len(f[i].buf);
	for (;;) {
		struct area a = frame(title, T(S_HINT_FORM));
		int y = a.y;
		if (text)
			y = draw_text(&a, y, A_NORMAL, text) + 1;
		int lw = 0;
		for (int i = 0; i < n; i++)
			if ((int)utf8_len(f[i].label) > lw)
				lw = (int)utf8_len(f[i].label);
		lw += 2;
		int fw = a.w - lw > 40 ? 40 : a.w - lw;
		if (fw < 8)
			fw = 8;
		int cy = -1, cx = 0;
		for (int i = 0; i < n; i++) {
			int row = y + i * 2, attr = i == focus ? A_FOCUS : A_FIELD;
			tui_put(row, a.x, i == focus ? A_BOLD : A_NORMAL, f[i].label, lw - 1);
			tui_fill(row, a.x + lw, fw, attr, ' ');
			size_t len = utf8_len(f[i].buf), off = f[i].pos >= (size_t)fw ? f[i].pos - (size_t)fw + 1 : 0;
			if (f[i].hidden)
				for (size_t c = off; c < len && c - off < (size_t)fw; c++)
					tui_putc(row, a.x + lw + (int)(c - off), attr, 0x2022);
			else
				tui_put(row, a.x + lw, attr, f[i].buf + utf8_prefix(f[i].buf, off), fw);
			if (i == focus) {
				cy = row;
				cx = a.x + lw + (int)(f[i].pos - off);
			}
		}
		if (err)
			draw_text(&a, y + n * 2, A_WARN, err);
		tui_cursor(cy, cx);
		tui_flush();
		int k = tui_key(-1);
		struct field *c = &f[focus];
		size_t len = utf8_len(c->buf);
		switch (k) {
		case K_TAB:
		case K_DOWN: focus = (focus + 1) % n; break;
		case K_BTAB:
		case K_UP: focus = (focus + n - 1) % n; break;
		case K_LEFT:
			if (c->pos)
				c->pos--;
			break;
		case K_RIGHT:
			if (c->pos < len)
				c->pos++;
			break;
		case K_HOME: c->pos = 0; break;
		case K_END: c->pos = len; break;
		case K_BS:
			if (c->pos) {
				buf_delete(c, c->pos - 1);
				c->pos--;
			}
			err = NULL;
			break;
		case K_DEL:
			if (c->pos < len)
				buf_delete(c, c->pos);
			err = NULL;
			break;
		case K_CTRL_U:
			c->buf[0] = '\0';
			c->pos = 0;
			err = NULL;
			break;
		case K_ENTER:
			if (focus < n - 1) {
				focus++;
				break;
			}
			err = check ? check(f, &focus) : NULL;
			if (!err) {
				tui_cursor(-1, 0);
				return NEXT;
			}
			break;
		case K_ESC: tui_cursor(-1, 0); return BACK;
		case K_CTRL_C:
			tui_cursor(-1, 0);
			if (want_quit())
				return QUIT;
			break;
		case K_EOF: return QUIT;
		default:
			if (k >= 0x20 && k < 0x110000) {
				buf_insert(c, (uint32_t)k);
				err = NULL;
			}
			break;
		}
	}
}

/* ---- the answers ---- */

static struct {
	int kmap, disk;
	char zone[96];
	bool encrypt, root_own;
	char luks[128], luks2[128], host[65], user[33], upw[128], upw2[128], rpw[128], rpw2[128];
	const char *profile, *font;
} st = {.kmap = 0, .disk = -1};

static void wipe_secrets(void)
{
	explicit_bzero(st.luks, sizeof(st.luks));
	explicit_bzero(st.luks2, sizeof(st.luks2));
	explicit_bzero(st.upw, sizeof(st.upw));
	explicit_bzero(st.upw2, sizeof(st.upw2));
	explicit_bzero(st.rpw, sizeof(st.rpw));
	explicit_bzero(st.rpw2, sizeof(st.rpw2));
}

static const char *check_password(const char *a, const char *b, size_t min)
{
	if (!*a)
		return T(S_ERR_EMPTY);
	for (const char *p = a; *p; p++)
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
			return T(S_ERR_ASCII);
	if (strlen(a) < min)
		return T(S_ERR_SHORT);
	if (strcmp(a, b))
		return T(S_ERR_MISMATCH);
	return NULL;
}

/* ---- the screens: NEXT, BACK or QUIT; DIR is where the last one went (skipped screens follow) ---- */

static int scr_lang(int dir)
{
	(void)dir;
	struct item it[2] = {{"English", NULL, NULL, false}, {"Русский", NULL, NULL, false}};
	int r = list_screen(T(S_LANG_TITLE), "Choose the language. · Выберите язык.", NULL, it, 2, lang, false);
	if (r < 0)
		return r == R_QUIT ? QUIT : BACK;
	lang = r;
	if (lang == 1 && tui_is_vt() && !tui_has_glyph(0x0436)) {
		lang = 0;
		const char *b[] = {"OK"};
		dialog("Русский", "The console font has no Cyrillic letters (kbd's consolefonts are missing): the installer "
				  "speaks English.", NULL, b, 1, 0);
	}
	if (lang == 1 && st.kmap == 0)
		st.kmap = 1;
	return NEXT;
}

static int scr_welcome(int dir)
{
	(void)dir;
	const char *b[] = {T(S_CONTINUE), T(S_BACK)};
	const char *warn = dry_run ? T(S_DRY_RUN) : !is_dir("/sys/firmware/efi") ? T(S_NO_UEFI) : NULL;
	int r = dialog(T(S_WELCOME_TITLE), T(S_WELCOME_TEXT), warn, b, 2, 0);
	return r == 0 ? NEXT : r == -2 ? QUIT : BACK;
}

static bool kmap_available(size_t i)
{
	char *dir = xasprintf("%s/usr/share/kbd/keymaps", strcmp(data_root, "/") ? data_root : "");
	bool kbd = is_dir(dir);
	free(dir);
	if (!kbd)
		return i == 0;
	char *f = kbd_file(data_root, "keymaps", kmaps[i].map);
	bool ok = f != NULL;
	free(f);
	return ok || i == 0;
}

static int scr_keymap(int dir)
{
	(void)dir;
	int n = 0, sel = 0, *idx = xmalloc(ARRAY_LEN(kmaps) * sizeof(int));
	struct item *it = xmalloc(ARRAY_LEN(kmaps) * sizeof(*it));
	for (size_t i = 0; i < ARRAY_LEN(kmaps); i++) {
		if (!kmap_available(i))
			continue;
		if ((int)i == st.kmap)
			sel = n;
		idx[n] = (int)i;
		it[n] = (struct item){xstrdup(lang ? kmaps[i].ru : kmaps[i].en), xstrdup(kmaps[i].map),
				      !strcmp(kmaps[i].map, "ruwin_alt_sh-UTF-8") ? xstrdup(T(S_KEYMAP_RU)) : NULL, false};
		n++;
	}
	char *warn = NULL;
	int result;
	for (;;) {
		int r = list_screen(T(S_KEYMAP_TITLE), T(S_KEYMAP_TEXT), warn, it, n, sel, true);
		if (r < 0) {
			result = r == R_QUIT ? QUIT : BACK;
			break;
		}
		sel = r;
		const struct kmap *k = &kmaps[idx[r]];
		free(warn);
		warn = NULL;
		if (tui_is_vt()) {
			const char *font = font_for(k->font);
			if (font && strcmp(font, live_font ? live_font : ""))
				set_font(font);
			if (quiet("loadkeys", "-q", k->map, (char *)NULL) != 0) {
				warn = tf(S_KEYMAP_FAIL, k->map);
				continue;
			}
		}
		st.kmap = idx[r];
		st.font = font_for(k->font);
		if (!st.font && lang == 1)
			st.font = font_for("cyr");
		result = NEXT;
		break;
	}
	free(warn);
	items_free(it, n);
	free(idx);
	return result;
}

static char *city_label(const char *zone)
{
	const char *c = strchr(zone, '/');
	char *s = xstrdup(c ? c + 1 : zone);
	sbuf b = SBUF_INIT;
	for (char *p = s; *p; p++) {
		if (*p == '_')
			sb_add(&b, " ");
		else if (*p == '/')
			sb_add(&b, " / ");
		else
			sb_addn(&b, p, 1);
	}
	free(s);
	return sb_take(&b);
}

static int scr_timezone(int dir)
{
	if (!zones.n)
		return dir;
	int nr = 0, reg_sel = 0, *ridx = xmalloc((ARRAY_LEN(regions) + 1) * sizeof(int));
	struct item *rit = xmalloc((ARRAY_LEN(regions) + 1) * sizeof(*rit));
	rit[nr] = (struct item){xstrdup(T(S_TZ_UTC)), xstrdup("UTC"), NULL, false};
	ridx[nr++] = -1;
	for (size_t i = 0; i < ARRAY_LEN(regions); i++) {
		size_t len = strlen(regions[i].name);
		bool any = false;
		for (size_t z = 0; z < zones.n && !any; z++)
			any = !strncmp(zones.v[z], regions[i].name, len) && zones.v[z][len] == '/';
		if (!any)
			continue;
		if (!strncmp(st.zone, regions[i].name, len) && st.zone[len] == '/')
			reg_sel = nr;
		rit[nr] = (struct item){xstrdup(T(regions[i].label)), lang ? xstrdup(regions[i].name) : NULL, NULL, false};
		ridx[nr++] = (int)i;
	}
	int result = BACK;
	for (;;) {
		int r = list_screen(T(S_TZ_TITLE), T(S_TZ_REGION_TEXT), NULL, rit, nr, reg_sel, true);
		if (r < 0) {
			result = r == R_QUIT ? QUIT : BACK;
			break;
		}
		reg_sel = r;
		if (ridx[r] < 0) {
			snprintf(st.zone, sizeof(st.zone), "UTC");
			result = NEXT;
			break;
		}
		const char *reg = regions[ridx[r]].name;
		size_t len = strlen(reg);
		int nc = 0, city_sel = 0;
		struct item *cit = xmalloc((zones.n + 1) * sizeof(*cit));
		int *cidx = xmalloc((zones.n + 1) * sizeof(int));
		for (size_t z = 0; z < zones.n; z++) {
			if (strncmp(zones.v[z], reg, len) || zones.v[z][len] != '/')
				continue;
			if (!strcmp(zones.v[z], st.zone))
				city_sel = nc;
			cit[nc] = (struct item){city_label(zones.v[z]), NULL, NULL, false};
			cidx[nc++] = (int)z;
		}
		char *title = xasprintf("%s: %s", T(S_TZ_TITLE), T(regions[ridx[r]].label));
		int c = list_screen(title, T(S_TZ_CITY_TEXT), NULL, cit, nc, city_sel, true);
		free(title);
		if (c >= 0)
			snprintf(st.zone, sizeof(st.zone), "%s", zones.v[cidx[c]]);
		items_free(cit, nc);
		free(cidx);
		if (c == R_QUIT) {
			result = QUIT;
			break;
		}
		if (c >= 0) {
			result = NEXT;
			break;
		}
	}
	items_free(rit, nr);
	free(ridx);
	return result;
}

static int scr_disk(int dir)
{
	(void)dir;
	for (;;) {
		load_disks();
		int n = (int)ndisks, sel = -1;
		struct item *it = xmalloc(((size_t)n + 1) * sizeof(*it));
		for (int i = 0; i < n; i++) {
			struct dinfo *d = &disks[i];
			char *label = *d->model ? xasprintf("%s  %s", d->dev, d->model) : xstrdup(d->dev);
			char *detail = *d->tran ? xasprintf("%s  %s", d->size, d->tran) : xstrdup(d->size);
			char *note;
			if (d->why == S_DISK_SMALL) {
				char *min = size_text(MIN_BYTES);
				note = tf(S_DISK_SMALL, min);
				free(min);
			} else if (d->why) {
				note = xstrdup(T(d->why));
			} else {
				note = d->content ? tf(S_DISK_HAS, d->content) : xstrdup(T(S_DISK_EMPTY));
			}
			it[i] = (struct item){label, detail, note, d->why != 0};
			if (sel < 0 && !d->why)
				sel = i;
			if (st.disk >= 0 && st.disk < n && i == st.disk && !d->why)
				sel = i;
		}
		it[n] = (struct item){xstrdup(T(S_DISK_RESCAN)), NULL, NULL, false};
		bool none = sel < 0;
		int r = list_screen(T(S_DISK_TITLE), T(S_DISK_TEXT), none ? T(S_DISK_NONE) : NULL, it, n + 1, none ? n : sel,
				    false);
		items_free(it, n + 1);
		if (r == R_QUIT)
			return QUIT;
		if (r == R_BACK)
			return BACK;
		if (r == n)
			continue;
		st.disk = r;
		return NEXT;
	}
}

static const char *check_luks(struct field *f, int *focus)
{
	const char *e = check_password(f[0].buf, f[1].buf, 8);
	*focus = e && e != T(S_ERR_MISMATCH) ? 0 : 1;
	return e;
}

static int scr_encrypt(int dir)
{
	(void)dir;
	struct item it[2] = {{(char *)T(S_ENC_NO), NULL, NULL, false}, {(char *)T(S_ENC_YES), "LUKS2", NULL, false}};
	for (;;) {
		int r = list_screen(T(S_ENC_TITLE), T(S_ENC_TEXT), NULL, it, 2, st.encrypt, false);
		if (r < 0)
			return r == R_QUIT ? QUIT : BACK;
		st.encrypt = r == 1;
		if (!st.encrypt)
			return NEXT;
		struct field f[2] = {{T(S_PASSPHRASE), st.luks, sizeof(st.luks), 0, true},
				     {T(S_REPEAT), st.luks2, sizeof(st.luks2), 0, true}};
		int fr = form_screen(T(S_PASS_TITLE), T(S_PASS_TEXT), f, 2, check_luks);
		if (fr != BACK)
			return fr;
	}
}

static bool user_taken(const char *name)
{
	char *pw = read_file("/etc/passwd", NULL);
	char *needle = xasprintf("\n%s:", name);
	bool taken = pw && (str_prefix(pw, needle + 1) || strstr(pw, needle));
	free(needle);
	free(pw);
	return taken;
}

static const char *check_user(struct field *f, int *focus)
{
	if (!valid_hostname(f[0].buf)) {
		*focus = 0;
		return T(S_ERR_HOST);
	}
	if (!valid_username(f[1].buf)) {
		*focus = 1;
		return T(S_ERR_USER);
	}
	if (user_taken(f[1].buf)) {
		*focus = 1;
		return T(S_ERR_TAKEN);
	}
	const char *e = check_password(f[2].buf, f[3].buf, 1);
	*focus = e && e != T(S_ERR_MISMATCH) ? 2 : 3;
	return e;
}

static int scr_user(int dir)
{
	(void)dir;
	if (!*st.host)
		snprintf(st.host, sizeof(st.host), "cleanlinux");
	struct field f[4] = {{T(S_HOSTNAME), st.host, sizeof(st.host), 0, false},
			     {T(S_USERNAME), st.user, sizeof(st.user), 0, false},
			     {T(S_PASSWORD), st.upw, sizeof(st.upw), 0, true},
			     {T(S_REPEAT), st.upw2, sizeof(st.upw2), 0, true}};
	return form_screen(T(S_USER_TITLE), T(S_USER_TEXT), f, 4, check_user);
}

static const char *check_root(struct field *f, int *focus)
{
	const char *e = check_password(f[0].buf, f[1].buf, 1);
	*focus = e && e != T(S_ERR_MISMATCH) ? 0 : 1;
	return e;
}

static int scr_root(int dir)
{
	(void)dir;
	for (;;) {
		char *same = tf(S_ROOT_SAME, st.user);
		struct item it[2] = {{same, NULL, NULL, false}, {(char *)T(S_ROOT_OWN), NULL, NULL, false}};
		int r = list_screen(T(S_ROOT_TITLE), T(S_ROOT_TEXT), NULL, it, 2, st.root_own, false);
		free(same);
		if (r < 0)
			return r == R_QUIT ? QUIT : BACK;
		st.root_own = r == 1;
		if (!st.root_own)
			return NEXT;
		struct field f[2] = {{T(S_PASSWORD), st.rpw, sizeof(st.rpw), 0, true},
				     {T(S_REPEAT), st.rpw2, sizeof(st.rpw2), 0, true}};
		int fr = form_screen(T(S_ROOT_PW_TITLE), T(S_ROOT_PW_TEXT), f, 2, check_root);
		if (fr != BACK)
			return fr;
	}
}

static int scr_profile(int dir)
{
	if (!prof_dev)
		return dir;
	char *a = tf(S_PROFILE_DEVICE, prof_dev), *b = tf(S_PROFILE_KEEP, prof_cur);
	struct item it[2] = {{a, NULL, NULL, false}, {b, NULL, NULL, false}};
	char *text = tf(S_PROFILE_TEXT, prof_model ? prof_model : prof_dev);
	int sel = st.profile || dir == NEXT ? 0 : 1;
	int r = list_screen(T(S_PROFILE_TITLE), text, NULL, it, 2, sel, false);
	free(a);
	free(b);
	free(text);
	if (r < 0)
		return r == R_QUIT ? QUIT : BACK;
	st.profile = r == 0 ? prof_dev : NULL;
	return NEXT;
}

static int scr_summary(int dir)
{
	(void)dir;
	const struct dinfo *d = &disks[st.disk];
	const struct kmap *k = &kmaps[st.kmap];
	sbuf b = SBUF_INIT;
	struct {
		int label;
		char *value;
	} rows[] = {
		{S_SUM_DISK, *d->model ? xasprintf("%s  %s  %s", d->dev, d->model, d->size) : xasprintf("%s  %s", d->dev, d->size)},
		{S_SUM_ENC, xstrdup(T(st.encrypt ? S_YES : S_NO))},
		{S_SUM_KEYMAP, xasprintf("%s (%s)", lang ? k->ru : k->en, k->map)},
		{S_SUM_TZ, xstrdup(*st.zone ? st.zone : "UTC")},
		{S_SUM_HOST, xstrdup(st.host)},
		{S_SUM_USER, xstrdup(st.user)},
		{S_SUM_ROOT, xstrdup(T(st.root_own ? S_ROOT_OWN_SHORT : S_ROOT_SAME_SHORT))},
		{S_SUM_PROFILE, xstrdup(st.profile ? st.profile : prof_cur ? prof_cur : "-")},
	};
	int lw = 0;
	for (size_t i = 0; i < ARRAY_LEN(rows); i++)
		if ((int)utf8_len(T(rows[i].label)) > lw)
			lw = (int)utf8_len(T(rows[i].label));
	for (size_t i = 0; i < ARRAY_LEN(rows); i++) {
		int pad = lw - (int)utf8_len(T(rows[i].label));
		sb_printf(&b, "%s:%*s %s\n", T(rows[i].label), pad, "", rows[i].value);
		free(rows[i].value);
	}
	char *text = sb_take(&b), *warn = tf(S_SUM_WARN, d->dev);
	char *q = tf(S_CONFIRM_TEXT, d->dev, *d->model ? d->model : d->size);
	const char *btn[] = {T(S_INSTALL), T(S_BACK)}, *yn[] = {T(S_BACK), T(S_ERASE)};
	int result;
	for (;;) {
		int r = dialog(T(S_SUM_TITLE), text, warn, btn, 2, 0);
		if (r != 0) {
			result = r == -2 ? QUIT : BACK;
			break;
		}
		/* the last question; Back (the default) returns to the summary */
		int saved = step_no;
		step_no = 0;
		int c = dialog(T(S_CONFIRM_TITLE), q, NULL, yn, 2, 0);
		step_no = saved;
		if (c == 1 || c == -2) {
			result = c == 1 ? NEXT : QUIT;
			break;
		}
	}
	free(q);
	free(text);
	free(warn);
	return result;
}

/* ---- the installation ---- */

/* console=, nomodeset and the drivers' modeset= of the running kernel: for the new system */
static char *carried_cmdline(void)
{
	char *c = read_line("/proc/cmdline");
	sbuf b = SBUF_INIT;
	sb_add(&b, "");
	for (char *save = NULL, *w = c ? strtok_r(c, " ", &save) : NULL; w; w = strtok_r(NULL, " ", &save)) {
		const char *dot = strchr(w, '.'), *eq = strchr(w, '=');
		bool modeset = dot && eq && dot < eq && !strncmp(dot, ".modeset=", 9);
		if (str_prefix(w, "console=") || !strcmp(w, "nomodeset") || modeset)
			sb_printf(&b, "%s%s", b.n ? " " : "", w);
	}
	free(c);
	return sb_take(&b);
}

static int write_secret(const char *dir, const char *name, const char *value, char **path)
{
	*path = xasprintf("%s/%s", dir, name);
	char *text = xasprintf("%s\n", value);
	int r = write_file(*path, text, strlen(text), 0600);
	explicit_bzero(text, strlen(text));
	free(text);
	return r;
}

static void remove_secret(char *path)
{
	if (!path)
		return;
	int fd = open(path, O_WRONLY);
	if (fd >= 0) {
		struct stat sb;
		if (fstat(fd, &sb) == 0 && sb.st_size > 0) {
			char zero[256] = {0};
			for (off_t left = sb.st_size; left > 0;) {
				ssize_t w = write(fd, zero, left > (off_t)sizeof(zero) ? sizeof(zero) : (size_t)left);
				if (w <= 0)
					break;
				left -= w;
			}
			fsync(fd);
		}
		close(fd);
	}
	unlink(path);
	free(path);
}

#define TAIL 64

struct tail {
	char *line[TAIL];
	int n;
};

static void tail_add(struct tail *t, const char *raw)
{
	char l[512];
	plain_line(l, sizeof(l), raw);
	if (!*l)
		return;
	if (t->n == TAIL) {
		free(t->line[0]);
		memmove(t->line, t->line + 1, (TAIL - 1) * sizeof(char *));
		t->n--;
	}
	t->line[t->n++] = xstrdup(l);
}

static void tail_free(struct tail *t)
{
	for (int i = 0; i < t->n; i++)
		free(t->line[i]);
	t->n = 0;
}

static const int step_names[] = {
	[STEP_NONE] = S_ST_DISK,     [STEP_DISK] = S_ST_DISK,	     [STEP_PARTITION] = S_ST_PARTITION,
	[STEP_ENCRYPT] = S_ST_ENCRYPT, [STEP_FORMAT] = S_ST_FORMAT,  [STEP_MOUNT] = S_ST_MOUNT,
	[STEP_PACKAGES] = S_ST_PACKAGES, [STEP_CONFIG] = S_ST_CONFIG, [STEP_USERS] = S_ST_USERS,
	[STEP_BOOT] = S_ST_BOOT,     [STEP_UNMOUNT] = S_ST_UNMOUNT, [STEP_DONE] = S_ST_DONE,
};

static void draw_progress(const struct dinfo *d, const struct progress *p, const struct tail *t)
{
	struct area a = frame(T(S_PROG_TITLE), T(S_HINT_WAIT));
	char *text = tf(S_PROG_TEXT, d->dev);
	int y = draw_text(&a, a.y, A_NORMAL, text) + 1;
	free(text);
	int bw = a.w - 7;
	int filled = bw * p->percent / 100;
	tui_fill(y, a.x, filled, A_BAR, 0x2588);
	tui_fill(y, a.x + filled, bw - filled, A_DIM, 0x2591);
	char pct[16];
	snprintf(pct, sizeof(pct), "%3d%%", p->percent);
	tui_put(y, a.x + bw + 2, A_BOLD, pct, 5);
	y += 2;
	char *what;
	if (p->step == STEP_PACKAGES && p->pkgs) {
		char *count = tf(S_PROG_PKGS, p->pkgs_done < p->pkgs ? p->pkgs_done : p->pkgs, p->pkgs);
		what = xasprintf("%s: %s%s%s", T(S_ST_PACKAGES), count, *p->pkg ? " · " : "", p->pkg);
		free(count);
	} else {
		what = xstrdup(T(step_names[p->step]));
	}
	tui_put(y, a.x, A_BOLD, what, a.w);
	free(what);
	y += 2;
	int rows = a.y + a.h - y;
	tui_hline(y - 1, a.x, a.w, A_DIM);
	for (int i = 0; i < rows && i < t->n; i++)
		tui_put(y + i, a.x, A_DIM, t->line[t->n - (rows < t->n ? rows : t->n) + i], a.w);
	tui_flush();
}

/* cleanlinux-install with the answers; its exit status (-1: it could not run) */
static int run_installer(struct tail *t)
{
	const struct dinfo *d = &disks[st.disk];
	char tmpl[] = "/run/cleanlinux-setup.XXXXXX";
	char *dir = mkdtemp(tmpl);
	char tmpl2[] = "/tmp/cleanlinux-setup.XXXXXX";
	if (!dir)
		dir = mkdtemp(tmpl2);
	if (!dir)
		return -1;
	char *rf = NULL, *uf = NULL, *lf = NULL;
	if (write_secret(dir, "root", st.root_own ? st.rpw : st.upw, &rf) < 0 || write_secret(dir, "user", st.upw, &uf) < 0 ||
	    (st.encrypt && write_secret(dir, "luks", st.luks, &lf) < 0)) {
		remove_secret(rf);
		remove_secret(uf);
		remove_secret(lf);
		rmdir(dir);
		return -1;
	}
	const char *installer = getenv("CLEANLINUX_SETUP_INSTALLER");
	strv a = STRV_INIT;
	strv_push(&a, installer && *installer ? installer : "cleanlinux-install");
	strv_push(&a, "-y");
	if (dry_run)
		strv_push(&a, "-n");
	strv_push(&a, "--hostname");
	strv_push(&a, st.host);
	strv_push(&a, "--user");
	strv_push(&a, st.user);
	strv_push(&a, "--root-password-file");
	strv_push(&a, rf);
	strv_push(&a, "--user-password-file");
	strv_push(&a, uf);
	if (st.encrypt) {
		strv_push(&a, "--encrypt");
		strv_push(&a, "--encrypt-password-file");
		strv_push(&a, lf);
	}
	if (*st.zone) {
		strv_push(&a, "--timezone");
		strv_push(&a, st.zone);
	}
	strv_push(&a, "--keymap");
	strv_push(&a, kmaps[st.kmap].map);
	if (st.font) {
		strv_push(&a, "--font");
		strv_push(&a, st.font);
	}
	if (st.profile) {
		strv_push(&a, "--profile");
		strv_push(&a, st.profile);
	}
	char *extra = carried_cmdline();
	if (*extra) {
		strv_push(&a, "--cmdline");
		strv_push(&a, extra);
	}
	free(extra);
	strv_push(&a, d->dev);

	int log = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	int rd = log >= 0 ? open(LOG_FILE, O_RDONLY | O_CLOEXEC) : -1;
	char *cmd = argv_text(strv_argv(&a));
	if (log >= 0) {
		char *head = xasprintf("cleanlinux-setup: %s\n", cmd);
		if (write(log, head, strlen(head)) < 0) {
			/* the log is a convenience */
		}
		free(head);
	}
	free(cmd);
	pid_t pid = log >= 0 && rd >= 0 ? fork() : -1;
	if (pid == 0) {
		/* its own session: a hangup of this terminal does not stop it */
		setsid();
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0)
			dup2(null, 0);
		dup2(log, 1);
		dup2(log, 2);
		execvp(strv_argv(&a)[0], strv_argv(&a));
		dprintf(2, "error: cannot run %s: %s\n", strv_argv(&a)[0], strerror(errno));
		_exit(127);
	}
	strv_free(&a);
	struct progress p = {0};
	int status = -1;
	if (pid > 0) {
		sbuf line = SBUF_INIT;
		bool running = true;
		struct timespec last = {0, 0};
		bool dirty = true;
		while (running) {
			char buf[4096];
			ssize_t n;
			while ((n = read(rd, buf, sizeof(buf))) > 0) {
				for (ssize_t i = 0; i < n; i++) {
					if (buf[i] == '\n' || buf[i] == '\r') {
						char *l = sb_take(&line);
						if (*l) {
							progress_line(&p, l);
							tail_add(t, l);
						}
						free(l);
					} else {
						sb_addn(&line, buf + i, 1);
					}
				}
				dirty = true;
			}
			int ws;
			pid_t w = waitpid(pid, &ws, WNOHANG);
			if (w == pid) {
				status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws);
				running = false;
				/* what it wrote last */
				while ((n = read(rd, buf, sizeof(buf))) > 0)
					for (ssize_t i = 0; i < n; i++) {
						if (buf[i] == '\n' || buf[i] == '\r') {
							char *l = sb_take(&line);
							if (*l) {
								progress_line(&p, l);
								tail_add(t, l);
							}
							free(l);
						} else {
							sb_addn(&line, buf + i, 1);
						}
					}
			}
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
			if ((dirty && ms >= 250) || !running) {
				draw_progress(d, &p, t);
				last = now;
				dirty = false;
			}
			if (running) {
				int k = tui_key(100);
				if (k == K_RESIZE)
					dirty = true;
			}
		}
		free(line.s);
	}
	if (log >= 0)
		close(log);
	if (rd >= 0)
		close(rd);
	remove_secret(rf);
	remove_secret(uf);
	remove_secret(lf);
	rmdir(dir);
	return status;
}

static int scr_install(int dir)
{
	(void)dir;
	struct tail t = {{0}, 0};
	step_no = 0;
	installing = true;
	int status = run_installer(&t);
	const struct dinfo *d = &disks[st.disk];
	int result;
	if (status == 0) {
		wipe_secrets();
		char *text = tf(S_DONE_TEXT, d->dev, st.user);
		const char *b[] = {T(S_REBOOT), T(S_POWEROFF), T(S_SHELL)};
		for (;;) {
			int r = dialog(T(S_DONE_TITLE), text, dry_run ? T(S_DRY_RUN) : NULL, b, 3, 0);
			if (r == 0 || r == 1) {
				if (dry_run)
					break;
				tui_end();
				sync();
				execlp(r == 0 ? "reboot" : "poweroff", r == 0 ? "reboot" : "poweroff", (char *)NULL);
				tui_begin(tui_ascii);
				continue;
			}
			if (r == 2 || r == -2)
				break;
		}
		free(text);
		result = QUIT;
	} else {
		sbuf b = SBUF_INIT;
		char *head = status < 0 ? tf(S_ERR_RUN, "cleanlinux-install") : tf(S_FAIL_TEXT, status, LOG_FILE);
		sb_printf(&b, "%s\n\n", head);
		free(head);
		int from = t.n > 10 ? t.n - 10 : 0;
		for (int i = from; i < t.n; i++)
			sb_printf(&b, "%s\n", t.line[i]);
		char *text = sb_take(&b);
		const char *btn[] = {T(S_RETRY), T(S_SHELL)};
		int r = dialog(T(S_FAIL_TITLE), text, NULL, btn, 2, 0);
		free(text);
		result = r == 1 || r == -2 ? QUIT : BACK;
	}
	tail_free(&t);
	installing = false;
	return result;
}

/* ---- main ---- */

/* the main console of the kernel (the last console=): /sys/class/tty/console/active lists them */
static bool on_main_console(void)
{
	char *act = read_line("/sys/class/tty/console/active");
	if (!act || !*act) {
		free(act);
		return true;
	}
	char *last = strrchr(act, ' ');
	char *main_tty = xstrdup(last ? last + 1 : act);
	free(act);
	if (!strcmp(main_tty, "tty0")) {
		free(main_tty);
		main_tty = read_line("/sys/class/tty/tty0/active");
		if (!main_tty)
			main_tty = xstrdup("tty1");
	}
	const char *me = ttyname(0);
	bool yes = me && str_prefix(me, "/dev/") && !strcmp(me + 5, main_tty);
	free(main_tty);
	return yes;
}

static _Noreturn void usage(int code)
{
	FILE *f = code ? stderr : stdout;
	fprintf(f, "usage: cleanlinux-setup [--lang en|ru] [--ascii] [--dry-run]\n"
		   "       cleanlinux-setup --auto\n"
		   "\n"
		   "Install CleanLinux on a disk of this computer, screen by screen: the language, the\n"
		   "keyboard layout, the time zone, the disk, encryption, the user and the passwords. The\n"
		   "work is done by cleanlinux-install; its log: " LOG_FILE ".\n"
		   "\n"
		   "      --lang en|ru  the language of the screens (the first one asks)\n"
		   "      --ascii       ASCII lines instead of box drawing\n"
		   "  -n, --dry-run     run cleanlinux-install -n: show what it would do\n"
		   "      --auto        the live system's autostart: only on the main console, not again\n"
		   "                    after it was quit\n"
		   "  -V, --version     print the version\n"
		   "  -h, --help        this help\n");
	exit(code);
}

int main(int argc, char **argv)
{
	prog = "cleanlinux-setup";
	enum { OPT_LANG = 256, OPT_ASCII, OPT_AUTO };
	static const struct option longopts[] = {
		{"lang", required_argument, NULL, OPT_LANG}, {"ascii", no_argument, NULL, OPT_ASCII},
		{"dry-run", no_argument, NULL, 'n'},	     {"auto", no_argument, NULL, OPT_AUTO},
		{"version", no_argument, NULL, 'V'},	     {"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	bool ascii = false, autostart = false;
	int c;
	while ((c = getopt_long(argc, argv, "nVh", longopts, NULL)) != -1) {
		switch (c) {
		case OPT_LANG:
			if (!strcmp(optarg, "ru"))
				lang = 1;
			else if (strcmp(optarg, "en"))
				die("--lang: en or ru");
			break;
		case OPT_ASCII: ascii = true; break;
		case 'n': dry_run = true; break;
		case OPT_AUTO: autostart = true; break;
		case 'V': printf("cleanlinux-setup %s\n", VERSION); return 0;
		case 'h': usage(0);
		default: usage(2);
		}
	}
	if (optind < argc)
		usage(2);
	const char *dr = getenv("CLEANLINUX_SETUP_DATA");
	if (dr && *dr)
		data_root = dr;
	if (autostart) {
		if (exists(EXIT_MARK) || !isatty(0))
			return 0;
		if (!on_main_console()) {
			printf("\nCleanLinux live: the installer runs on the main console; here: cleanlinux-setup\n"
			       "CleanLinux live: установщик открыт на основной консоли; здесь: cleanlinux-setup\n\n");
			return 0;
		}
	}
	if (!isatty(0) || !isatty(1))
		die("cleanlinux-setup needs a terminal");
	if (geteuid() != 0 && !dry_run)
		die("run it as root (or try --dry-run)");
	int lock = open(geteuid() == 0 ? LOCK_FILE : "/tmp/cleanlinux-setup.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) < 0) {
		if (autostart)
			return 0;
		die("the installer is running on another console already");
	}
	load_zones();
	load_profiles();
	char *lt = realpath("/etc/localtime", NULL);
	const char *z = lt ? strstr(lt, "zoneinfo/") : NULL;
	if (z && valid_zone(z + 9))
		snprintf(st.zone, sizeof(st.zone), "%s", z + 9);
	free(lt);

	tui_begin(ascii);
	if (tui_is_vt()) {
		big_screen = tui_cols >= 300;
		/* Latin and Cyrillic (and a larger font on a large screen): the language screen
		 * shows both */
		set_font(font_for("cyr"));
	}
	static int (*const screens[])(int) = {scr_lang, scr_welcome, scr_keymap, scr_timezone, scr_disk, scr_encrypt,
					      scr_user, scr_root, scr_profile, scr_summary, scr_install};
	int sc = 0, dir = NEXT;
	while (sc < (int)ARRAY_LEN(screens)) {
		step_no = sc < STEPS ? sc + 1 : 0;
		if (sc == 3 && !*st.zone)
			snprintf(st.zone, sizeof(st.zone), "%s", lang ? "Europe/Moscow" : "UTC");
		int r = screens[sc](dir);
		if (r == QUIT)
			break;
		dir = r;
		if (r == NEXT)
			sc++;
		else if (sc > 0)
			sc--;
	}
	wipe_secrets();
	tui_end();
	if (geteuid() == 0) {
		int fd = open(EXIT_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0)
			close(fd);
	}
	printf("cleanlinux-setup: %s\n", lang ? "снова запустить установщик: cleanlinux-setup"
					    : "to start the installer again: cleanlinux-setup");
	return 0;
}
