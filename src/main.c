/* SPDX-License-Identifier: MIT */
/* cleanlinux-install: put the running CleanLinux on a disk.
 *
 *   cleanlinux-install [options] DISK      erase DISK: GPT, ESP (vfat) + btrfs (@ @home @snapshots)
 *   cleanlinux-install [options] -t DIR    install into filesystems already mounted at DIR
 *
 * The packages are the very builds the running system has installed (burn clone, from the
 * package cache, nothing is built); then fstab by PARTUUID, the kernel command line, hostname,
 * passwords and a user, network services, the kernel and systemd-boot in the ESP and an EFI
 * boot entry. No desktop or other choices: those come with spins on top of the base. */
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

static struct {
	const char *disk, *target, *hostname, *user, *root_pw_file, *user_pw_file, *profile, *cmdline;
	strv packages;
	unsigned esp_mib;
	bool yes, dry_run, keep_mounted, no_efi_entry, target_given;
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
		   "Install the running CleanLinux: the same package builds (burn clone), configured for the\n"
		   "new disk. With DISK everything on it is erased: GPT, an EFI system partition (vfat, /boot)\n"
		   "and btrfs with the subvolumes @ (/), @home and @snapshots, compressed with zstd. Without\n"
		   "DISK the filesystems must already be mounted at --target (an ESP at /boot, /efi or /boot/efi).\n"
		   "\n"
		   "  -t, --target DIR            mount point of the new system (default /mnt)\n"
		   "  -H, --hostname NAME         host name (default cleanlinux)\n"
		   "  -u, --user NAME             create a user in the group wheel\n"
		   "      --root-password-file F  the root password: the first line of F (- for stdin);\n"
		   "                              otherwise it is asked on the terminal\n"
		   "      --user-password-file F  the same for the user\n"
		   "  -p, --packages \"A B\"        clone only these packages and their dependencies\n"
		   "                              (default: every package of the running system)\n"
		   "  -P, --profile NAME          the burn profile of the new system (default: the running one)\n"
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
	strv_split(&need, "burn cleanlinux-boot");
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
	/* the burn settings of the running system (its profile), unless one is given */
	char *conf = read_file("/etc/burn/burn.conf", NULL);
	p = tpath("/etc/burn/burn.conf");
	if (!conf)
		conf = read_file(p, NULL);
	if (o.profile) {
		char *c = conf_set(conf ? conf : "", "PROFILE", o.profile);
		free(conf);
		conf = c;
	}
	if (conf)
		put_file(p, conf, 0644, o.profile != NULL);
	free(conf);
	free(p);
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
	if (exists_already)
		info("user %s exists already", o.user);
	else
		must_run(NULL, "useradd", "useradd", "-R", o.target, "-m", "-U", "-G", "wheel", "-s", "/bin/bash", o.user, NULL);
	set_password(o.user, user_hash);
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

static void bootloader(const char *esp, const char *esp_dev, const char *esp_partuuid)
{
	msg("Kernel and systemd-boot in the EFI system partition (%s)", esp);
	must_run(NULL, "cleanlinux-boot kernel sync", "cleanlinux-boot", "--root", o.target, "kernel", "sync", NULL);
	if (!o.dry_run) {
		char *pat = xasprintf("%s/loader/entries/*.conf", esp);
		glob_t g;
		int r = glob(pat, 0, NULL, &g);
		free(pat);
		if (r != 0)
			die("no boot entry in %s/loader/entries", esp);
		globfree(&g);
		pat = xasprintf("%s/EFI/BOOT/BOOT*.EFI", esp);
		r = glob(pat, 0, NULL, &g);
		free(pat);
		if (r != 0)
			die("no systemd-boot in %s/EFI/BOOT (is systemd-boot installed?)", esp);
		globfree(&g);
	}
	if (o.no_efi_entry || !*EFI_ARCH)
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
	char *pw = file ? read_password_file(file) : ask_password(who);
	char *h = hash_password(pw);
	explicit_bzero(pw, strlen(pw));
	free(pw);
	return h;
}

int main(int argc, char **argv)
{
	enum { OPT_ROOT_PW = 256, OPT_USER_PW, OPT_CMDLINE, OPT_ESP, OPT_NO_EFI, OPT_KEEP };
	static const struct option longopts[] = {
		{"target", required_argument, NULL, 't'},
		{"hostname", required_argument, NULL, 'H'},
		{"user", required_argument, NULL, 'u'},
		{"root-password-file", required_argument, NULL, OPT_ROOT_PW},
		{"user-password-file", required_argument, NULL, OPT_USER_PW},
		{"packages", required_argument, NULL, 'p'},
		{"profile", required_argument, NULL, 'P'},
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
	while ((c = getopt_long(argc, argv, "t:H:u:p:P:ynVh", longopts, NULL)) != -1) {
		switch (c) {
		case 't': o.target = optarg, o.target_given = true; break;
		case 'H': o.hostname = optarg; break;
		case 'u': o.user = optarg; break;
		case OPT_ROOT_PW: o.root_pw_file = optarg; break;
		case OPT_USER_PW: o.user_pw_file = optarg; break;
		case 'p': strv_split(&o.packages, optarg); break;
		case 'P': o.profile = optarg; break;
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
	if (o.user_pw_file && !o.user)
		die("--user-password-file needs --user");
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
	if (o.packages.n) {
		char *t = argv_text(o.packages.v);
		info("packages: %s and their dependencies", t);
		free(t);
	} else {
		info("packages: all of the running system");
	}
	if (o.profile)
		info("burn profile %s", o.profile);
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
					die("%s (the root) has no PARTUUID: it must be a partition (LUKS and LVM need an "
					    "initramfs, not supported by the installer yet)",
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

	/* the command line first: the kernel package's trigger writes the boot entry with it */
	msg("Kernel command line");
	char *cl = cmdline_text(root_spec + strlen("PARTUUID="), rootflags, o.cmdline);
	char *clf = xasprintf("%s\n", cl), *clp = tpath("/etc/kernel/cmdline");
	put_file(clp, clf, 0644, true);
	free(clf);
	free(clp);
	free(cl);

	clone_system();
	write_config(&fstab);
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
	return 0;
}
