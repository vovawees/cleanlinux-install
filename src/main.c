/* SPDX-License-Identifier: MIT */
/* cleanlinux-install: put CleanLinux on a disk.
 *
 *   cleanlinux-install [options] DISK      erase DISK: GPT, ESP (vfat) + btrfs (@ @home @snapshots)
 *   cleanlinux-install [options] -t DIR    install into filesystems already mounted at DIR
 *
 * The system is the one of the live USB image, copied as it is (nothing is built or downloaded):
 * its system tree @/ and the layers over it, the proprietary drivers (nonfree/, unless
 * --no-nonfree) and a spin (spins/NAME/ with --spin NAME: a desktop). Without a live medium, or
 * with --clone, burn clone installs the packages of the running system instead. Then fstab by
 * PARTUUID, the kernel command line, hostname, passwords and a user, the time zone and the
 * console's keymap and font, burn's settings (the device profile, ALLOW_NONFREE), network
 * services, and the kernel for the boot loader of the device in the ESP (systemd-boot and an EFI
 * boot entry; U-Boot extlinux, the Raspberry Pi firmware or an Android boot image when the
 * profile says so). */
#define _GNU_SOURCE
#include "install.h"

#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#define EFI_ARCH "x64"
#define EFI_ARCH_UP "X64"
#elif defined(__aarch64__)
#define EFI_ARCH "aa64"
#define EFI_ARCH_UP "AA64"
#elif defined(__riscv) && __riscv_xlen == 64
#define EFI_ARCH "riscv64"
#define EFI_ARCH_UP "RISCV64"
#elif defined(__i386__)
#define EFI_ARCH "ia32"
#define EFI_ARCH_UP "IA32"
#else
#define EFI_ARCH ""
#define EFI_ARCH_UP ""
#endif

/* the live system's medium (cleanlinux-boot mounts the partition of the USB image there) */
#define LIVE_MEDIUM "/run/cleanlinux/live/medium"

static struct {
	const char *disk, *target, *hostname, *user, *root_pw_file, *user_pw_file, *profile, *cmdline;
	const char *timezone, *keymap, *font, *source, *spin;
	strv packages;
	unsigned esp_mib;
	bool yes, dry_run, keep_mounted, no_efi_entry, target_given, no_nonfree, clone, autologin;
} o = {.target = "/mnt", .hostname = "cleanlinux", .esp_mib = 512};

static strv mounted; /* mount points made here, in order */
static bool finished;

static void unmount_all(void)
{
	if (mounted.n)
		sync();
	for (size_t i = mounted.n; i-- > 0;)
		if (umount2(mounted.v[i], 0) < 0)
			warn_errno("umount %s", mounted.v[i]);
	strv_free(&mounted);
}

static void at_exit(void)
{
	if (o.keep_mounted) {
		if (!finished && mounted.n)
			warnx("the new system stays mounted at %s (--keep-mounted)", o.target);
		return;
	}
	unmount_all();
}

static _Noreturn void usage(int code)
{
	FILE *f = code ? stderr : stdout;
	fprintf(f, "usage: cleanlinux-install [options] DISK\n"
		   "       cleanlinux-install [options] --target DIR\n"
		   "\n"
		   "Install CleanLinux: the system of the live USB image, copied as it is (nothing is built\n"
		   "or downloaded), with the proprietary drivers and, if asked, a spin (a desktop). DISK is\n"
		   "erased: GPT, an EFI system partition, btrfs with the subvolumes @ (/), @home and\n"
		   "@snapshots, compressed with zstd. Without DISK the filesystems must already be mounted at\n"
		   "--target (an ESP at /boot, /efi or /boot/efi).\n"
		   "\n"
		   "  -t, --target DIR            mount point of the new system (default /mnt)\n"
		   "  -H, --hostname NAME         host name (default cleanlinux)\n"
		   "  -u, --user NAME             create a user in the group wheel\n"
		   "      --root-password-file F  the root password: the first line of F (- for stdin);\n"
		   "                              asked on the terminal when not given\n"
		   "      --user-password-file F  the same for the user\n"
		   "      --autologin             the user logs in on tty1 by itself, without a password\n"
		   "                              (handhelds: no keyboard to type one; a spin starts there)\n"
		   "  -s, --spin NAME             also the spin NAME of the live medium (spins/NAME: labwc)\n"
		   "      --no-nonfree            without the proprietary drivers (the layer nonfree/ of the\n"
		   "                              medium), and burn builds none (ALLOW_NONFREE=0)\n"
		   "      --source DIR            the system tree to copy (default: the live medium's\n"
		   "                              " LIVE_MEDIUM "/@); its layers are next to it\n"
		   "      --clone                 burn clone of the running system's packages instead of a copy\n"
		   "                              (what happens without a live medium)\n"
		   "  -p, --packages \"A B\"        burn clone of only these packages and their dependencies\n"
		   "  -P, --profile NAME          the burn profile of the new system (default: the running one;\n"
		   "                              burn profile detect suggests one for this computer)\n"
		   "      --timezone ZONE         the time zone: Europe/Moscow, America/New_York, UTC\n"
		   "                              (default: the one of the running system)\n"
		   "      --keymap MAP            the console keymap of kbd: us, de-latin1, ruwin_alt_sh-UTF-8\n"
		   "      --font FONT             the console font of kbd: latarcyrheb-sun16, lat2-16\n"
		   "                              (default for both: those of the running system)\n"
		   "      --cmdline \"ARGS\"        more kernel arguments (root= and rootflags= are written)\n"
		   "      --esp-size MIB          size of the EFI system partition (default 512)\n"
		   "      --no-efi-entry          do not add an EFI boot entry (the fallback loader still boots)\n"
		   "      --keep-mounted          leave the new system mounted at the end\n"
		   "  -y, --yes                   do not ask before erasing DISK\n"
		   "  -n, --dry-run               show what would be done and change nothing\n"
		   "  -V, --version               print the version\n"
		   "  -h, --help                  this help\n");
	exit(code);
}

/* ---- actions: printed, and done unless --dry-run ---- */

static int act_run(char *const argv[], const char *input)
{
	char *t = argv_text(argv);
	info("$ %s", t);
	free(t);
	if (input && o.dry_run) {
		char *copy = xstrdup(input);
		for (char *save = NULL, *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
			info("  %s", l);
		free(copy);
	}
	if (o.dry_run)
		return 0;
	return run(argv, input);
}

static void must_run(const char *input, const char *what, ...)
{
	strv a = STRV_INIT;
	va_list ap;
	va_start(ap, what);
	const char *s;
	while ((s = va_arg(ap, const char *)))
		strv_push(&a, s);
	va_end(ap);
	int r = act_run(strv_argv(&a), input);
	if (r != 0)
		die("%s failed (%s)", what, r < 0 ? "cannot run it" : "exit status");
	strv_free(&a);
}

static void put_file(const char *path, const char *text, mode_t mode, bool show)
{
	info("write %s", path);
	if (o.dry_run) {
		if (show) {
			char *copy = xstrdup(text);
			for (char *save = NULL, *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
				info("  %s", l);
			free(copy);
		}
		return;
	}
	char *dir = xstrdup(path);
	*strrchr(dir, '/') = '\0';
	if (mkdir_p(dir, 0755) < 0)
		die_errno("mkdir %s", dir);
	free(dir);
	if (write_file(path, text, strlen(text), mode) < 0)
		die_errno("write %s", path);
}

static void do_mount(const char *src, const char *dir, const char *type, unsigned long flags, const char *data)
{
	info("mount %s %s (%s%s%s)", src, dir, type, data ? ", " : "", data ? data : "");
	if (o.dry_run)
		return;
	if (mkdir_p(dir, 0755) < 0)
		die_errno("mkdir %s", dir);
	if (mount(src, dir, type, flags, data) < 0)
		die_errno("mount %s on %s", src, dir);
	strv_push(&mounted, dir);
}

static char *tpath(const char *path) { return xasprintf("%s%s", strcmp(o.target, "/") ? o.target : "", path); }

/* ---- the steps ---- */

static void check_tools(void)
{
	strv need = STRV_INIT;
	strv_push(&need, "cleanlinux-boot");
	if (o.clone)
		strv_push(&need, "burn");
	if (o.disk)
		strv_split(&need, "sfdisk mkfs.vfat mkfs.btrfs");
	if (o.user)
		strv_push(&need, "useradd");
	for (size_t i = 0; i < need.n; i++) {
		char *p = find_program(need.v[i]);
		if (!p)
			die("%s is missing: the installer runs on a CleanLinux system (base)", need.v[i]);
		free(p);
	}
	strv_free(&need);
}

/* the ESP below the target: a vfat mount point at /boot, /efi or /boot/efi */
static char *find_esp(void)
{
	static const char *const dirs[] = {"/boot", "/efi", "/boot/efi"};
	for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
		char *p = tpath(dirs[i]);
		struct statfs sf;
		if (is_mountpoint(p) && statfs(p, &sf) == 0 && sf.f_type == MSDOS_SUPER_MAGIC)
			return p;
		free(p);
	}
	return NULL;
}

static void partition(const struct disk *d, const char *esp_uuid, const char *root_uuid)
{
	struct utsname u;
	uname(&u);
	msg("Partitioning %s: GPT, EFI system partition %u MiB + btrfs root", d->dev, o.esp_mib);
	char *script = sfdisk_script(o.esp_mib, esp_uuid, root_uuid, root_part_type(u.machine));
	must_run(script, "sfdisk", "sfdisk", "--quiet", "--wipe", "always", "--wipe-partitions", "always", d->dev, NULL);
	free(script);
	char *udevadm = find_program("udevadm");
	if (udevadm && !o.dry_run)
		runl(NULL, "udevadm", "settle", "--timeout=10", NULL);
	free(udevadm);
}

static void make_filesystems(const char *esp_dev, const char *root_dev)
{
	msg("Formatting: %s vfat, %s btrfs", esp_dev, root_dev);
	must_run(NULL, "mkfs.vfat", "mkfs.vfat", "-F", "32", "-n", "ESP", esp_dev, NULL);
	must_run(NULL, "mkfs.btrfs", "mkfs.btrfs", "-f", "-q", "-L", "CleanLinux", root_dev, NULL);
	/* the subvolumes, in the top level of the new filesystem */
	static const char *const subvols[] = {"@", "@home", "@snapshots"};
	info("btrfs subvolumes: @ (/), @home (/home), @snapshots (/.snapshots)");
	if (o.dry_run)
		return;
	char tmpl[] = "/run/cleanlinux-install.XXXXXX";
	char *top = mkdtemp(tmpl);
	if (!top) {
		strcpy(tmpl, "/tmp/cleanlinux-install.XXXXXX");
		if (!(top = mkdtemp(tmpl)))
			die_errno("mkdtemp");
	}
	if (mount(root_dev, top, "btrfs", 0, NULL) < 0)
		die_errno("mount %s", root_dev);
	for (size_t i = 0; i < ARRAY_LEN(subvols); i++)
		if (btrfs_subvol_create(top, subvols[i]) < 0) {
			int e = errno;
			umount2(top, 0);
			errno = e;
			die_errno("btrfs subvolume %s", subvols[i]);
		}
	if (umount2(top, 0) < 0)
		die_errno("umount %s", top);
	rmdir(top);
}

static void mount_target(const char *esp_dev, const char *root_dev)
{
	msg("Mounting the new system at %s", o.target);
	do_mount(root_dev, o.target, "btrfs", MS_NOATIME, "subvol=@,compress=zstd:1");
	char *home = tpath("/home"), *snap = tpath("/.snapshots"), *boot = tpath("/boot");
	do_mount(root_dev, home, "btrfs", MS_NOATIME, "subvol=@home,compress=zstd:1");
	do_mount(root_dev, snap, "btrfs", MS_NOATIME, "subvol=@snapshots,compress=zstd:1");
	do_mount(esp_dev, boot, "vfat", MS_NOATIME, "umask=0077");
	free(home);
	free(snap);
	free(boot);
}

/* ---- the system: a copy of the live medium's, or burn clone ---- */

struct layer {
	char *dir, *what; /* the tree; "the system", "the proprietary drivers", "the spin labwc" */
	struct tree_size size;
};
static struct layer layers[3];
static size_t nlayers;

static void add_layer(const char *dir, const char *what)
{
	layers[nlayers].dir = xstrdup(dir);
	layers[nlayers].what = xstrdup(what);
	tree_measure(dir, &layers[nlayers].size);
	nlayers++;
}

/* the source and its layers: --source DIR (or the live medium's @) with nonfree/ and
 * spins/NAME/ next to it */
static void find_layers(void)
{
	char *src = o.source ? xstrdup(o.source) : xstrdup(LIVE_MEDIUM "/@");
	char *usr = xasprintf("%s/usr", src);
	if (!is_dir(usr))
		die("%s has no system (%s): --source names the root of a system tree", src, usr);
	free(usr);
	add_layer(src, "the system");
	char *base = xstrdup(src);
	char *slash = strrchr(base, '/');
	if (slash && slash != base)
		*slash = '\0';
	else
		strcpy(base, "/");
	char *nonfree = xasprintf("%s/nonfree", base);
	if (!o.no_nonfree && is_dir(nonfree))
		add_layer(nonfree, "the proprietary drivers");
	if (o.spin) {
		char *sp = xasprintf("%s/spins/%s", base, o.spin), *what = xasprintf("the spin %s", o.spin);
		if (!is_dir(sp))
			die("--spin %s: the medium has no such spin (%s)", o.spin, sp);
		add_layer(sp, what);
		free(sp);
		free(what);
	}
	free(nonfree);
	free(base);
	free(src);
}

struct copy_state {
	uint64_t total, base;
	int last;
	bool tty;
};

static void copy_progress(uint64_t bytes, uint64_t entries, void *ctx)
{
	(void)entries;
	struct copy_state *c = ctx;
	uint64_t done = c->base + bytes;
	int pct = c->total ? (int)(done * 100 / c->total) : 100;
	if (pct > 100)
		pct = 100;
	if (pct == c->last)
		return;
	c->last = pct;
	char *d = size_text(done), *t = size_text(c->total);
	printf("%s==> Copied %d%% (%s of %s)%s", c->tty ? "\r" : "", pct, d, t, c->tty ? "\033[K" : "\n");
	fflush(stdout);
	free(d);
	free(t);
}

static void copy_system(void)
{
	uint64_t total = 0, entries = 0;
	for (size_t i = 0; i < nlayers; i++) {
		total += layers[i].size.bytes;
		entries += layers[i].size.entries;
	}
	char *t = size_text(total);
	msg("Copying the system (%s in %llu files, nothing is built or downloaded)", t, (unsigned long long)entries);
	free(t);
	struct copy_state cs = {.total = total, .last = -1, .tty = isatty(1)};
	for (size_t i = 0; i < nlayers; i++) {
		char *sz = size_text(layers[i].size.bytes);
		info("%s: %s (%s)", layers[i].what, layers[i].dir, sz);
		free(sz);
		if (o.dry_run)
			continue;
		int bad = tree_copy(layers[i].dir, o.target, copy_progress, &cs);
		if (bad)
			die("%d files of %s could not be copied", bad, layers[i].dir);
		cs.base += layers[i].size.bytes;
	}
	if (cs.tty && !o.dry_run)
		printf("\n");
	if (o.dry_run)
		return;
	char *pat = tpath("/usr/lib/modules/*/vmlinuz");
	glob_t g;
	int gr = glob(pat, 0, NULL, &g);
	free(pat);
	if (gr != 0)
		die("no kernel in the copied system (/usr/lib/modules/*/vmlinuz)");
	globfree(&g);
}

static void clone_system(void)
{
	msg("Installing the packages of the running system (burn clone, nothing is built)");
	strv a = STRV_INIT;
	strv_split(&a, "burn -r");
	strv_push(&a, o.target);
	strv_split(&a, "-y clone /");
	for (size_t i = 0; i < o.packages.n; i++)
		strv_push(&a, o.packages.v[i]);
	int r = act_run(strv_argv(&a), NULL);
	strv_free(&a);
	if (r != 0)
		die("burn clone failed: the package files of the running system are in /var/cache/burn/packages");
	if (o.dry_run)
		return;
	char *pat = tpath("/usr/lib/modules/*/vmlinuz");
	glob_t g;
	int gr = glob(pat, 0, NULL, &g);
	free(pat);
	if (gr != 0)
		die("no kernel among the installed packages: add linux to --packages");
	globfree(&g);
}

/* burn profile detect: the device profile of this computer, when it is not the one the running
 * system (the live image: generic) builds for; asked on a terminal, else only shown */
static void suggest_profile(void)
{
	if (o.profile)
		return;
	char *const argv[] = {"burn", "profile", "detect", NULL};
	char *out = run_output(argv);
	char *det = detected_profile(out);
	free(out);
	if (!det)
		return;
	char *conf = read_file("/etc/burn/burn.conf", NULL);
	char *cur = conf ? conf_get(conf, "PROFILE") : NULL;
	free(conf);
	if (!cur || !*cur) {
		free(cur);
		cur = xstrdup("native"); /* burn's default */
	}
	if (strcmp(det, cur)) {
		msg("Hardware");
		info("burn profile detect: %s (the running system builds for %s)", det, cur);
		if (!o.yes && !o.dry_run && isatty(0)) {
			char *q = xasprintf("Use the profile %s for the new system (burn update then rebuilds it for this "
					    "computer)? Type yes:",
					    det);
			if (ask_yes(q)) {
				o.profile = det;
				det = NULL;
			}
			free(q);
		} else {
			info("--profile %s makes it the profile of the new system", det);
		}
	}
	free(det);
	free(cur);
}

static void write_config(const fsents *fstab)
{
	msg("Configuring the new system");
	char *t = fstab_text(fstab), *p = tpath("/etc/fstab");
	put_file(p, t, 0644, true);
	free(t);
	free(p);
	t = xasprintf("%s\n", o.hostname);
	p = tpath("/etc/hostname");
	put_file(p, t, 0644, true);
	free(t);
	free(p);
	/* burn's settings: those of the copied system (of the running one for burn clone), with the
	 * profile given and whether it builds proprietary packages */
	p = tpath("/etc/burn/burn.conf");
	char *conf = o.clone || o.dry_run ? read_file("/etc/burn/burn.conf", NULL) : read_file(p, NULL);
	if (!conf)
		conf = o.clone ? read_file(p, NULL) : read_file("/etc/burn/burn.conf", NULL);
	if (o.profile) {
		char *c = conf_set(conf ? conf : "", "PROFILE", o.profile);
		free(conf);
		/* the firmware list of the image gives way to the profile's own */
		conf = conf_unset(c, "FIRMWARE");
		free(c);
	}
	char *c = conf_set(conf ? conf : "", "ALLOW_NONFREE", o.no_nonfree ? "0" : "1");
	free(conf);
	conf = c;
	info("burn: %s", o.no_nonfree ? "no proprietary packages (ALLOW_NONFREE=0)"
				       : "proprietary drivers allowed (ALLOW_NONFREE=1)");
	put_file(p, conf, 0644, o.profile != NULL);
	free(conf);
	free(p);
}

/* /etc/localtime and /etc/vconsole.conf: the choices, else those of the running system */
static void console_config(void)
{
	char *lt_live = NULL;
	char buf[4096];
	ssize_t n = readlink("/etc/localtime", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		const char *z = strstr(buf, "zoneinfo/");
		if (z && valid_zone(z + 9))
			lt_live = xstrdup(z + 9);
	}
	const char *zone = o.timezone ? o.timezone : lt_live;
	char *vc_live = read_file("/etc/vconsole.conf", NULL);
	if (!zone && !o.keymap && !o.font && !vc_live) {
		free(lt_live);
		return;
	}
	msg("Time zone and console");
	if (zone) {
		char *rel = xasprintf("../usr/share/zoneinfo/%s", zone), *zi = xasprintf("/usr/share/zoneinfo/%s", zone);
		char *zp = tpath(zi), *lt = tpath("/etc/localtime");
		if (!o.dry_run && !is_file(zp)) {
			warnx("the time zone %s is not in the new system (%s; the tzdata package): the clock stays in UTC",
			      zone, zi);
		} else {
			info("time zone %s: /etc/localtime -> %s", zone, rel);
			if (!o.dry_run) {
				if (unlink(lt) < 0 && errno != ENOENT)
					die_errno("unlink %s", lt);
				if (symlink(rel, lt) < 0)
					die_errno("symlink %s", lt);
			}
		}
		free(rel);
		free(zi);
		free(zp);
		free(lt);
	}
	if (o.keymap || o.font || vc_live) {
		char *p = tpath("/etc/vconsole.conf");
		char *text = vc_live ? xstrdup(vc_live) : read_file(p, NULL);
		if (!text)
			text = xstrdup("# the keymap and font of the console (cleanlinux-boot vconsole)\n");
		static const char *const kinds[][2] = {{"KEYMAP", "keymaps"}, {"FONT", "consolefonts"}};
		const char *vals[] = {o.keymap, o.font};
		for (size_t i = 0; i < ARRAY_LEN(vals); i++) {
			if (!vals[i])
				continue;
			char *f = o.dry_run ? NULL : kbd_file(o.target, kinds[i][1], vals[i]);
			char *dir = xasprintf("%s/usr/share/kbd/%s", o.target, kinds[i][1]);
			if (!o.dry_run && !f && is_dir(dir))
				warnx("%s %s: kbd has no such %s in the new system", kinds[i][0], vals[i],
				      i ? "font" : "keymap");
			free(dir);
			free(f);
			char *t = conf_set(text, kinds[i][0], vals[i]);
			free(text);
			text = t;
		}
		put_file(p, text, 0644, true);
		free(text);
		free(p);
	}
	free(vc_live);
	free(lt_live);
}

static void set_password(const char *user, const char *hash)
{
	char *p = tpath("/etc/shadow");
	info("password of %s in %s", user, p);
	if (o.dry_run) {
		free(p);
		return;
	}
	struct stat st;
	mode_t mode = stat(p, &st) == 0 ? st.st_mode & 07777 : 0600;
	char *old = read_file(p, NULL);
	char *text = shadow_set(old ? old : "", user, hash, (long)(time(NULL) / 86400));
	if (write_file(p, text, strlen(text), mode) < 0)
		die_errno("write %s", p);
	explicit_bzero(text, strlen(text));
	free(text);
	free(old);
	free(p);
}

static void users(const char *root_hash, const char *user_hash)
{
	msg("Users");
	set_password("root", root_hash);
	if (!o.user)
		return;
	char *pw = tpath("/etc/passwd");
	char *passwd = read_file(pw, NULL);
	char *needle = xasprintf("\n%s:", o.user);
	bool exists_already = passwd && (str_prefix(passwd, needle + 1) || strstr(passwd, needle));
	free(needle);
	free(passwd);
	free(pw);
	if (exists_already) {
		info("user %s exists already", o.user);
	} else {
		char *gp = tpath("/etc/group"), *gt = read_file(gp, NULL), *groups = user_groups(gt);
		must_run(NULL, "useradd", "useradd", "-R", o.target, "-m", "-U", "-G", groups, "-s", "/bin/bash", o.user, NULL);
		free(groups);
		free(gt);
		free(gp);
	}
	set_password(o.user, user_hash);
	/* cleanlinux-boot's getty logs the user in on tty1 (cleanlinux.autologin=0 turns it off) */
	char *al = tpath("/etc/cleanlinux/autologin");
	if (o.autologin) {
		char *t = xasprintf("# the user who logs in on tty1 by itself (cleanlinux-boot getty); empty: nobody\n%s\n",
				    o.user);
		put_file(al, t, 0644, true);
		free(t);
	} else if (exists(al)) {
		info("remove %s", al);
		if (!o.dry_run && unlink(al) < 0)
			warn_errno("remove %s", al);
	}
	free(al);
}

static void enable_service(const char *name)
{
	char *svc = tpath("/usr/lib/dinit.d/"), *link = tpath("/etc/dinit.d/boot.d/");
	char *s = xasprintf("%s%s", svc, name), *l = xasprintf("%s%s", link, name);
	free(svc);
	free(link);
	if (!exists(s) || (!o.dry_run && exists(l))) {
		free(s);
		free(l);
		return;
	}
	info("enable %s (/etc/dinit.d/boot.d)", name);
	if (!o.dry_run) {
		char *dir = tpath("/etc/dinit.d/boot.d");
		mkdir_p(dir, 0755);
		free(dir);
		char *dest = xasprintf("/usr/lib/dinit.d/%s", name);
		if (symlink(dest, l) < 0 && errno != EEXIST)
			die_errno("symlink %s", l);
		free(dest);
	}
	free(s);
	free(l);
}

static void services(void)
{
	msg("Services");
	enable_service("dhcpcd");
	/* Wi-Fi when the machine has it, with the networks known to the running system */
	glob_t g;
	if (glob("/sys/class/net/*/wireless", 0, NULL, &g) == 0) {
		enable_service("iwd");
		globfree(&g);
		if (glob("/var/lib/iwd/*", 0, NULL, &g) == 0) {
			char *dir = tpath("/var/lib/iwd");
			for (size_t i = 0; i < g.gl_pathc; i++) {
				const char *base = strrchr(g.gl_pathv[i], '/') + 1;
				if (!str_suffix(base, ".psk") && !str_suffix(base, ".open") && !str_suffix(base, ".8021x"))
					continue;
				char *dst = xasprintf("%s/%s", dir, base);
				info("Wi-Fi network %s", base);
				size_t len;
				char *data = o.dry_run ? NULL : read_file(g.gl_pathv[i], &len);
				if (data && (mkdir_p(dir, 0700) < 0 || write_file(dst, data, len, 0600) < 0))
					warn_errno("%s", dst);
				free(data);
				free(dst);
			}
			free(dir);
			globfree(&g);
		}
	}
}

static bool have_glob(const char *dir, const char *pattern)
{
	char *pat = xasprintf("%s/%s", dir, pattern);
	glob_t g;
	int r = glob(pat, 0, NULL, &g);
	free(pat);
	if (r == 0)
		globfree(&g);
	return r == 0;
}

/* the boot loaders of the new system: loader= of cleanlinux-boot kernel config (its boot.d, from the
 * package hardware of the profile); systemd-boot when it does not say */
static void target_loaders(strv *out)
{
	char *cfg = NULL;
	if (!o.dry_run) {
		char *argv[] = {"cleanlinux-boot", "--root", (char *)o.target, "kernel", "config", NULL};
		cfg = run_output(argv);
	}
	char *loader = cfg ? conf_get(cfg, "loader") : NULL;
	strv_split(out, loader && *loader ? loader : "systemd-boot");
	free(loader);
	free(cfg);
}

static void bootloader(const char *esp, const char *esp_dev, const char *esp_partuuid)
{
	strv loaders = STRV_INIT;
	target_loaders(&loaders);
	char *names = argv_text(strv_argv(&loaders));
	msg("Kernel and boot loader (%s) in the boot partition (%s)", names, esp);
	free(names);
	must_run(NULL, "cleanlinux-boot kernel sync", "cleanlinux-boot", "--root", o.target, "kernel", "sync", NULL);
	bool sdboot = strv_contains(&loaders, "systemd-boot");
	if (!o.dry_run) {
		if (sdboot && !have_glob(esp, "loader/entries/*.conf"))
			die("no boot entry in %s/loader/entries", esp);
		if (sdboot && !have_glob(esp, "EFI/BOOT/BOOT*.EFI"))
			die("no systemd-boot in %s/EFI/BOOT (is systemd-boot installed?)", esp);
		if (strv_contains(&loaders, "extlinux") && !have_glob(esp, "extlinux/extlinux.conf"))
			die("no %s/extlinux/extlinux.conf for U-Boot", esp);
		if (strv_contains(&loaders, "rpi") && !have_glob(esp, "config.txt"))
			die("no %s/config.txt for the Raspberry Pi firmware", esp);
		if (strv_contains(&loaders, "android")) {
			char *dir = xasprintf("%s/var/lib/cleanlinux/android", o.target);
			if (!have_glob(dir, "*.img"))
				die("no Android boot image in %s (is devicetree= set in /etc/cleanlinux/boot.d?)", dir);
			info("Android boot image in /var/lib/cleanlinux/android: flash it into the boot partition of the device");
			info("(android.partition= in /etc/cleanlinux/boot.d and cleanlinux-boot kernel sync --flash, or fastboot flash boot)");
			free(dir);
		}
	}
	strv_free(&loaders);
	if (!sdboot || o.no_efi_entry || !*EFI_ARCH)
		return;
	if (!is_dir("/sys/firmware/efi/efivars")) {
		info("no EFI variables (not booted with UEFI): the firmware finds EFI/BOOT/BOOT%s.EFI", EFI_ARCH_UP);
		return;
	}
	/* the disk and the partition number of the ESP */
	struct stat st;
	if (o.dry_run || !esp_partuuid || stat(esp_dev, &st) < 0) {
		info("EFI boot entry \"CleanLinux\": \\EFI\\systemd\\systemd-boot%s.efi", EFI_ARCH);
		return;
	}
	char *link = xasprintf("/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
	char *psys = realpath(link, NULL);
	free(link);
	unsigned partno = psys ? (unsigned)sysfs_u64(psys, "partition") : 0;
	if (!partno) {
		warnx("%s is not a partition: no EFI boot entry", esp_dev);
		free(psys);
		return;
	}
	*strrchr(psys, '/') = '\0';
	struct disk d = {0};
	char *disk_dev = xasprintf("/dev/%s", strrchr(psys, '/') + 1);
	disk_open(disk_dev, &d, false);
	if (efi_boot_entry(&d, partno, esp_partuuid + strlen("PARTUUID="), "/EFI/systemd/systemd-boot" EFI_ARCH ".efi",
			   "CleanLinux") < 0)
		warn_errno("EFI boot entry not written (the firmware still finds EFI/BOOT/BOOT%s.EFI)", EFI_ARCH_UP);
	free(disk_dev);
	free(psys);
}

static char *get_password(const char *who, const char *file)
{
	if (o.dry_run)
		return xstrdup("$y$dry-run");
	char *opt = xasprintf("--%s-password-file", strcmp(who, "root") ? "user" : "root");
	char *pw = file ? read_password_file(file) : ask_password(who, opt);
	free(opt);
	char *h = hash_password(pw);
	explicit_bzero(pw, strlen(pw));
	free(pw);
	return h;
}

int main(int argc, char **argv)
{
	enum { OPT_ROOT_PW = 256, OPT_USER_PW, OPT_CMDLINE, OPT_ESP, OPT_NO_EFI, OPT_KEEP, OPT_TZ, OPT_KEYMAP, OPT_FONT,
	       OPT_SOURCE, OPT_NO_NONFREE, OPT_NONFREE, OPT_CLONE, OPT_AUTOLOGIN };
	static const struct option longopts[] = {
		{"target", required_argument, NULL, 't'},
		{"hostname", required_argument, NULL, 'H'},
		{"user", required_argument, NULL, 'u'},
		{"root-password-file", required_argument, NULL, OPT_ROOT_PW},
		{"user-password-file", required_argument, NULL, OPT_USER_PW},
		{"autologin", no_argument, NULL, OPT_AUTOLOGIN},
		{"spin", required_argument, NULL, 's'},
		{"no-nonfree", no_argument, NULL, OPT_NO_NONFREE},
		{"nonfree", no_argument, NULL, OPT_NONFREE},
		{"source", required_argument, NULL, OPT_SOURCE},
		{"clone", no_argument, NULL, OPT_CLONE},
		{"packages", required_argument, NULL, 'p'},
		{"profile", required_argument, NULL, 'P'},
		{"timezone", required_argument, NULL, OPT_TZ},
		{"keymap", required_argument, NULL, OPT_KEYMAP},
		{"font", required_argument, NULL, OPT_FONT},
		{"cmdline", required_argument, NULL, OPT_CMDLINE},
		{"esp-size", required_argument, NULL, OPT_ESP},
		{"no-efi-entry", no_argument, NULL, OPT_NO_EFI},
		{"keep-mounted", no_argument, NULL, OPT_KEEP},
		{"yes", no_argument, NULL, 'y'},
		{"dry-run", no_argument, NULL, 'n'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "t:H:u:s:p:P:ynVh", longopts, NULL)) != -1) {
		switch (c) {
		case 't': o.target = optarg, o.target_given = true; break;
		case 'H': o.hostname = optarg; break;
		case 'u': o.user = optarg; break;
		case OPT_ROOT_PW: o.root_pw_file = optarg; break;
		case OPT_USER_PW: o.user_pw_file = optarg; break;
		case OPT_AUTOLOGIN: o.autologin = true; break;
		case 's': o.spin = optarg; break;
		case OPT_NO_NONFREE: o.no_nonfree = true; break;
		case OPT_NONFREE: o.no_nonfree = false; break;
		case OPT_SOURCE: o.source = optarg; break;
		case OPT_CLONE: o.clone = true; break;
		case 'p': strv_split(&o.packages, optarg); break;
		case 'P': o.profile = optarg; break;
		case OPT_TZ: o.timezone = optarg; break;
		case OPT_KEYMAP: o.keymap = optarg; break;
		case OPT_FONT: o.font = optarg; break;
		case OPT_CMDLINE: o.cmdline = optarg; break;
		case OPT_ESP: {
			char *end;
			unsigned long v = strtoul(optarg, &end, 10);
			if (*end || v < 64 || v > 8192)
				die("--esp-size: a size in MiB between 64 and 8192");
			o.esp_mib = (unsigned)v;
			break;
		}
		case OPT_NO_EFI: o.no_efi_entry = true; break;
		case OPT_KEEP: o.keep_mounted = true; break;
		case 'y': o.yes = true; break;
		case 'n': o.dry_run = true; break;
		case 'V': printf("cleanlinux-install %s\n", VERSION); return 0;
		case 'h': usage(0);
		default: usage(2);
		}
	}
	if (optind < argc)
		o.disk = argv[optind++];
	if (optind < argc || (!o.disk && !o.target_given))
		usage(2);
	if (!valid_hostname(o.hostname))
		die("--hostname %s: letters, digits, '-' and '.', up to 64", o.hostname);
	if (o.user && !valid_username(o.user))
		die("--user %s: lower case letters, digits, '_' and '-', starting with a letter, up to 32", o.user);
	if (o.timezone && !valid_zone(o.timezone))
		die("--timezone %s: a zone of tzdata such as Europe/Berlin or UTC", o.timezone);
	if (o.keymap && !valid_console_name(o.keymap))
		die("--keymap %s: the name of a kbd keymap such as us or de-latin1", o.keymap);
	if (o.font && !valid_console_name(o.font))
		die("--font %s: the name of a kbd console font such as lat2-16", o.font);
	if (o.spin && !valid_console_name(o.spin))
		die("--spin %s: the name of a spin such as labwc", o.spin);
	if (o.user_pw_file && !o.user)
		die("--user-password-file needs --user");
	if (o.autologin && !o.user)
		die("--autologin needs --user");
	if (o.packages.n)
		o.clone = true;
	if (o.clone && o.source)
		die("--source copies a system tree, --clone and --packages install packages: one of them");
	/* no live medium: the packages of the running system */
	if (!o.clone && !o.source && !is_dir(LIVE_MEDIUM "/@/usr")) {
		if (o.spin)
			die("--spin %s: spins come from the live medium (%s/spins), and there is none", o.spin, LIVE_MEDIUM);
		o.clone = true;
	}
	if (o.clone && o.spin)
		die("--spin needs the copy of a live medium: burn spin add %s adds it to an installed system", o.spin);
	if (o.root_pw_file && o.user_pw_file && !strcmp(o.root_pw_file, "-") && !strcmp(o.user_pw_file, "-"))
		die("only one password can come from stdin");
	if (o.target[0] != '/')
		die("--target %s: an absolute path", o.target);
	char *real = realpath(o.target, NULL);
	if (real)
		o.target = real;
	if (!strcmp(o.target, "/"))
		die("--target /: the running system itself");
	if (geteuid() != 0 && !o.dry_run)
		die("run it as root");
	atexit(at_exit);
	check_tools();
	if (!o.clone)
		find_layers();

	struct disk d = {0};
	char *esp = NULL;
	if (o.disk) {
		disk_open(o.disk, &d, geteuid() == 0);
		if (d.bytes < ((uint64_t)o.esp_mib << 20) + (3ULL << 30))
			die("%s is too small: at least %u MiB", d.dev, o.esp_mib + 3072);
		if (is_mountpoint(o.target))
			die("%s is a mount point already: unmount it, or install into it without a disk", o.target);
		msg("Disk");
		disk_describe(&d);
	} else {
		if (!is_mountpoint(o.target))
			die("%s is not a mount point: mount the root filesystem of the new system there, or give a disk", o.target);
		if (!(esp = find_esp()))
			die("no EFI system partition (vfat) mounted at %s/boot, %s/efi or %s/boot/efi", o.target, o.target, o.target);
	}
	suggest_profile();
	msg("The new system");
	info("hostname %s, %s%s", o.hostname, o.user ? "user " : "only root", o.user ? o.user : "");
	if (o.clone && o.packages.n) {
		char *t = argv_text(o.packages.v);
		info("packages: %s and their dependencies (burn clone)", t);
		free(t);
	} else if (o.clone) {
		info("packages: all of the running system (burn clone)");
	} else {
		for (size_t i = 0; i < nlayers; i++)
			info("%s: %s", layers[i].what, layers[i].dir);
	}
	info("proprietary drivers: %s", o.no_nonfree ? "no" : "yes");
	if (o.profile)
		info("burn profile %s", o.profile);
	if (o.timezone || o.keymap || o.font)
		info("time zone %s, keymap %s, font %s", o.timezone ? o.timezone : "-", o.keymap ? o.keymap : "-",
		     o.font ? o.font : "-");
	if (o.disk && !o.yes && !o.dry_run) {
		char *q = xasprintf("Erase everything on %s and install CleanLinux? Type yes:", d.dev);
		if (!ask_yes(q))
			die("aborted, nothing changed");
		free(q);
	}
	/* passwords before anything changes */
	char *root_hash = get_password("root", o.root_pw_file);
	char *user_hash = o.user ? get_password(o.user, o.user_pw_file) : NULL;

	fsents fstab = {0};
	char *root_spec = NULL, *rootflags = NULL, *esp_dev = NULL, *esp_spec = NULL;
	if (o.disk) {
		char esp_uuid[37], root_uuid[37];
		uuid_random(esp_uuid);
		uuid_random(root_uuid);
		partition(&d, esp_uuid, root_uuid);
		esp_dev = disk_part_dev(&d, 1, o.dry_run ? 0 : 10000);
		char *root_dev = disk_part_dev(&d, 2, o.dry_run ? 0 : 10000);
		if (!esp_dev || !root_dev)
			die("the new partitions of %s did not appear", d.dev);
		make_filesystems(esp_dev, root_dev);
		mount_target(esp_dev, root_dev);
		root_spec = xasprintf("PARTUUID=%s", root_uuid);
		esp_spec = xasprintf("PARTUUID=%s", esp_uuid);
		rootflags = xstrdup("subvol=@");
		fsents_add(&fstab, root_spec, "/", "btrfs", "subvol=@," BTRFS_OPTS, 0);
		fsents_add(&fstab, root_spec, "/home", "btrfs", "subvol=@home," BTRFS_OPTS, 0);
		fsents_add(&fstab, root_spec, "/.snapshots", "btrfs", "subvol=@snapshots," BTRFS_OPTS, 0);
		fsents_add(&fstab, esp_spec, "/boot", "vfat", ESP_OPTS, 2);
		esp = tpath("/boot");
		free(root_dev);
	} else {
		char *mi = read_file("/proc/self/mountinfo", NULL);
		if (!mi)
			die_errno("/proc/self/mountinfo");
		fsents raw = {0};
		mountinfo_fsents(mi, o.target, NULL, NULL, &raw);
		free(mi);
		const char *esp_rel = esp + strlen(o.target);
		for (size_t i = 0; i < raw.n; i++) {
			char *spec = partuuid_of(raw.v[i].spec, NULL);
			if (!strcmp(raw.v[i].dir, "/")) {
				if (!spec)
					die("%s (the root) has no PARTUUID: it must be a partition (LVM and device-mapper "
					    "are not supported)",
					    raw.v[i].spec);
				root_spec = xstrdup(spec);
				const char *sv = strstr(raw.v[i].opts, "subvol=");
				if (sv && !strcmp(raw.v[i].type, "btrfs"))
					rootflags = xstrndup(sv, strcspn(sv, ","));
			}
			if (!strcmp(raw.v[i].dir, esp_rel)) {
				esp_dev = xstrdup(raw.v[i].spec);
				esp_spec = spec ? xstrdup(spec) : NULL;
			}
			if (!spec)
				warnx("%s has no PARTUUID: fstab names it by its device path", raw.v[i].spec);
			fsents_add(&fstab, spec ? spec : raw.v[i].spec, raw.v[i].dir, raw.v[i].type, raw.v[i].opts, raw.v[i].pass);
			free(spec);
		}
		fsents_free(&raw);
		if (!root_spec)
			die("%s: no block device mounted there", o.target);
	}
	bool tmp = false;
	for (size_t i = 0; i < fstab.n; i++)
		tmp = tmp || !strcmp(fstab.v[i].dir, "/tmp");
	if (!tmp)
		fsents_add(&fstab, "tmpfs", "/tmp", "tmpfs", "nosuid,nodev", 0);

	/* the command line before burn clone: the kernel package's trigger writes the boot entry
	 * with it; after a copy, which brings none */
	char *cl = cmdline_text(root_spec, rootflags, o.cmdline);
	char *clf = xasprintf("%s\n", cl), *clp = tpath("/etc/kernel/cmdline");
	if (o.clone) {
		msg("Kernel command line");
		put_file(clp, clf, 0644, true);
		clone_system();
	} else {
		copy_system();
		msg("Kernel command line");
		put_file(clp, clf, 0644, true);
	}
	free(clf);
	free(clp);
	free(cl);
	write_config(&fstab);
	console_config();
	users(root_hash, user_hash);
	services();
	bootloader(esp, esp_dev, esp_spec);
	finished = true;
	if (!o.keep_mounted && !o.dry_run) {
		msg("Unmounting %s", o.target);
		unmount_all();
	}
	if (o.dry_run)
		msg("Dry run: nothing was changed");
	else
		msg("Done: CleanLinux is on %s. Remove the installation medium and reboot.", o.disk ? d.dev : o.target);
	fsents_free(&fstab);
	free(root_hash);
	free(user_hash);
	free(root_spec);
	free(esp_spec);
	free(esp_dev);
	free(rootflags);
	free(esp);
	for (size_t i = 0; i < nlayers; i++) {
		free(layers[i].dir);
		free(layers[i].what);
	}
	return 0;
}
