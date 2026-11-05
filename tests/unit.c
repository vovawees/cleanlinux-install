/* SPDX-License-Identifier: MIT */
/* Unit tests of the pure parts of the installer (src/plan.c), of the copy of system trees
 * (src/copy.c, in a temporary directory) and of the TUI (text in src/tui.c). */
#define _GNU_SOURCE
#include "install.h"
#include "tui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <unistd.h>

static int checks, failed;

#define CHECK(cond, ...)                                                                                     \
	do {                                                                                                 \
		checks++;                                                                                    \
		if (!(cond)) {                                                                               \
			failed++;                                                                            \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                 \
			fprintf(stderr, __VA_ARGS__);                                                        \
			fputc('\n', stderr);                                                                 \
		}                                                                                            \
	} while (0)

static void check_str(const char *what, char *got, const char *want)
{
	checks++;
	if (got && want ? strcmp(got, want) != 0 : got != want) {
		failed++;
		fprintf(stderr, "FAIL %s:\n--- got\n%s\n--- want\n%s\n", what, got ? got : "(null)",
			want ? want : "(null)");
	}
	free(got);
}

static char *fake_spec(const char *dev, void *ctx)
{
	(void)ctx;
	if (!strcmp(dev, "/dev/nvme0n1p2"))
		return xstrdup("PARTUUID=2222");
	if (!strcmp(dev, "/dev/nvme0n1p1"))
		return xstrdup("PARTUUID=1111");
	return NULL;
}

static void put(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
	if (fd >= 0)
		close(fd);
	chmod(path, mode);
}

static char *got(const char *path)
{
	return read_file(path, NULL);
}

static void progress_cb(uint64_t bytes, uint64_t entries, void *ctx)
{
	(void)bytes;
	*(uint64_t *)ctx = entries;
}

/* tree_copy: files, modes, times, symbolic and hard links, directories merged, whiteouts (with
 * the right to make device nodes) */
static void test_tree_copy(void)
{
	char tmpl[] = "/tmp/cli-unit.XXXXXX";
	char *t = mkdtemp(tmpl);
	CHECK(t != NULL, "mkdtemp");
	if (!t)
		return;
	char *src = xasprintf("%s/src", t), *dst = xasprintf("%s/dst", t), *p;
#define P(...) (free(p), p = xasprintf(__VA_ARGS__))
	p = NULL;
	mkdir(src, 0755);
	mkdir(dst, 0755);
	mkdir(P("%s/etc", src), 0755);
	put(P("%s/etc/hostname", src), "cleanlinux-live\n", 0644);
	put(P("%s/etc/shadow", src), "root::1::::::\n", 0600);
	mkdir(P("%s/usr", src), 0755);
	mkdir(P("%s/usr/bin", src), 0711);
	put(P("%s/usr/bin/tool", src), "#!/bin/sh\n", 0755);
	char *a1 = xasprintf("%s/usr/bin/tool", src), *a2 = xasprintf("%s/usr/bin/tool2", src);
	CHECK(link(a1, a2) == 0, "link");
	free(a1);
	free(a2);
	CHECK(symlink("tool", P("%s/usr/bin/t", src)) == 0, "symlink");
	struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}};
	utimes(P("%s/etc/hostname", src), tv);
	/* what DST has: merged with, or removed by a whiteout */
	mkdir(P("%s/etc", dst), 0755);
	put(P("%s/etc/keep", dst), "kept\n", 0644);
	put(P("%s/etc/hostname", dst), "old\n", 0644);
	put(P("%s/etc/gone", dst), "x\n", 0644);
	bool whiteout = mknod(P("%s/etc/gone", src), S_IFCHR | 0000, makedev(0, 0)) == 0;

	uint64_t entries = 0;
	struct tree_size sz = {0, 0};
	tree_measure(src, &sz);
	CHECK(sz.entries >= 7 && sz.bytes == strlen("cleanlinux-live\n") + strlen("root::1::::::\n") + 2 * strlen("#!/bin/sh\n"),
	      "tree_measure: %llu entries, %llu bytes", (unsigned long long)sz.entries, (unsigned long long)sz.bytes);
	int fails = tree_copy(src, dst, progress_cb, &entries);
	CHECK(fails == 0, "tree_copy: %d failed", fails);
	check_str("copied file", got(P("%s/etc/hostname", dst)), "cleanlinux-live\n");
	check_str("kept file", got(P("%s/etc/keep", dst)), "kept\n");
	struct stat s1, s2;
	CHECK(stat(P("%s/etc/shadow", dst), &s1) == 0 && (s1.st_mode & 07777) == 0600, "mode 0600: %o", s1.st_mode & 07777);
	CHECK(stat(P("%s/usr/bin", dst), &s1) == 0 && (s1.st_mode & 07777) == 0711, "directory mode: %o", s1.st_mode & 07777);
	CHECK(stat(P("%s/etc/hostname", dst), &s1) == 0 && s1.st_mtime == 1000000000, "mtime: %lld", (long long)s1.st_mtime);
	CHECK(stat(P("%s/usr/bin/tool", dst), &s1) == 0 && stat(P("%s/usr/bin/tool2", dst), &s2) == 0 &&
		      s1.st_ino == s2.st_ino && s1.st_nlink == 2,
	      "hard link");
	char lt[16] = "";
	CHECK(readlink(P("%s/usr/bin/t", dst), lt, sizeof(lt) - 1) == 4 && !strcmp(lt, "tool"), "symlink: %s", lt);
	if (whiteout)
		CHECK(lstat(P("%s/etc/gone", dst), &s1) < 0 && errno == ENOENT, "the whiteout removed etc/gone");
	else
		printf("unit: no whiteout test (mknod needs root)\n");
	CHECK(entries > 0, "progress called");
	/* again over itself: nothing fails */
	CHECK(tree_copy(src, dst, NULL, NULL) == 0, "tree_copy again");
	char *rm = xasprintf("rm -rf '%s'", t);
	if (system(rm) != 0)
		fprintf(stderr, "cannot remove %s\n", t);
	free(rm);
	free(p);
	free(src);
	free(dst);
#undef P
}

int main(void)
{
	/* partition names */
	check_str("part sda", part_name("sda", 1), "sda1");
	check_str("part vdb", part_name("vdb", 2), "vdb2");
	check_str("part nvme", part_name("nvme0n1", 1), "nvme0n1p1");
	check_str("part mmc", part_name("mmcblk0", 2), "mmcblk0p2");
	check_str("part loop", part_name("loop0", 1), "loop0p1");

	/* root partition types of the discoverable partitions spec */
	CHECK(!strcmp(root_part_type("x86_64"), "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709"), "x86_64 root type");
	CHECK(!strcmp(root_part_type("aarch64"), "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"), "aarch64 root type");
	CHECK(!strcmp(root_part_type("mips"), "0FC63DAF-8483-4772-8E79-3D69D8477DE4"), "fallback type");

	check_str("sfdisk script",
		  sfdisk_script(512, "11111111-2222-4333-8444-555555555555", "66666666-7777-4888-9999-aaaaaaaaaaaa",
				root_part_type("x86_64")),
		  "label: gpt\n"
		  "size=512MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=11111111-2222-4333-8444-555555555555, "
		  "name=\"EFI system partition\"\n"
		  "type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, uuid=66666666-7777-4888-9999-aaaaaaaaaaaa, "
		  "name=\"CleanLinux\"\n");

	/* random UUIDs: version 4, the RFC 4122 variant, all different */
	char a[37], b[37];
	uuid_random(a);
	uuid_random(b);
	CHECK(uuid_valid(a) && uuid_valid(b), "uuid format %s", a);
	CHECK(a[14] == '4' && strchr("89ab", a[19]), "uuid version/variant %s", a);
	CHECK(strcmp(a, b), "uuids differ");
	CHECK(!uuid_valid("1234") && !uuid_valid("11111111-2222-4333-8444-55555555555g"), "uuid_valid rejects");

	/* GUIDs in EFI byte order: the first three fields little endian */
	uint8_t g[16];
	CHECK(guid_bytes("00112233-4455-6677-8899-aabbccddeeff", g) == 0, "guid parse");
	static const uint8_t want_g[16] = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
					   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	CHECK(!memcmp(g, want_g, 16), "guid byte order");

	/* an EFI load option: attributes, the path list length, UCS-2 description, HD + file + end */
	uint8_t opt[512];
	size_t n = efi_load_option(opt, sizeof(opt), "CL", 1, 2048, 1048576, g, "/EFI/systemd/systemd-bootx64.efi");
	const char *path = "\\EFI\\systemd\\systemd-bootx64.efi";
	size_t fl = 4 + 2 * (strlen(path) + 1), fpl = 42 + fl + 4;
	CHECK(n == 4 + 2 + 6 + fpl, "load option size %zu", n);
	CHECK(opt[0] == 1 && opt[1] == 0 && opt[2] == 0 && opt[3] == 0, "LOAD_OPTION_ACTIVE");
	CHECK((size_t)(opt[4] | opt[5] << 8) == fpl, "FilePathListLength");
	CHECK(opt[6] == 'C' && opt[7] == 0 && opt[8] == 'L' && opt[10] == 0 && opt[11] == 0, "description");
	const uint8_t *hd = opt + 12;
	CHECK(hd[0] == 4 && hd[1] == 1 && hd[2] == 42 && hd[3] == 0, "HD node header");
	CHECK(hd[4] == 1 && hd[8] == 0x00 && hd[9] == 0x08 && hd[18] == 0x10, "partition number, start, size");
	CHECK(!memcmp(hd + 24, g, 16) && hd[40] == 2 && hd[41] == 2, "GPT signature");
	const uint8_t *fp = hd + 42;
	CHECK(fp[0] == 4 && fp[1] == 4 && (size_t)(fp[2] | fp[3] << 8) == fl, "file node header");
	CHECK(fp[4] == '\\' && fp[6] == 'E' && fp[12] == '\\', "file path with backslashes");
	const uint8_t *end = fp + fl;
	CHECK(end[0] == 0x7f && end[1] == 0xff && end[2] == 4 && end[3] == 0, "end node");
	CHECK(efi_load_option(opt, 40, "CL", 1, 0, 0, g, "/x") == 0, "load option: buffer too small");

	/* fstab from mountinfo: only below the target, PARTUUIDs, btrfs subvolumes and options */
	const char *mi =
		"22 1 0:21 / / rw,relatime shared:1 - btrfs /dev/sda2 rw,compress=zstd:1,subvolid=256,subvol=/@\n"
		"60 22 0:40 / /mnt rw,noatime shared:30 - btrfs /dev/nvme0n1p2 rw,compress=zstd:3,ssd,space_cache=v2,subvolid=256,subvol=/@\n"
		"61 60 0:40 / /mnt/home rw,noatime shared:31 - btrfs /dev/nvme0n1p2 rw,compress=zstd:3,subvolid=257,subvol=/@home\n"
		"62 60 259:1 / /mnt/boot rw,noatime shared:32 - vfat /dev/nvme0n1p1 rw,fmask=0077,dmask=0077\n"
		"63 60 8:17 / /mnt/srv\\040data rw,relatime shared:33 - ext4 /dev/sdb1 rw\n"
		"64 60 0:50 / /mnt/run rw shared:34 - tmpfs tmpfs rw\n"
		"65 1 0:51 / /mntx rw shared:35 - ext4 /dev/sdc1 rw\n";
	fsents f = {0};
	int cnt = mountinfo_fsents(mi, "/mnt/", fake_spec, NULL, &f);
	CHECK(cnt == 4 && f.n == 4, "mountinfo entries: %d", cnt);
	check_str("fstab", fstab_text(&f),
		  "# /etc/fstab, written by cleanlinux-install. Partitions by PARTUUID: the names survive\n"
		  "# reformatting and disk order (ls -l /dev/disk/by-partuuid).\n"
		  "# <device> <mount point> <type> <options> <dump> <pass>\n"
		  "PARTUUID=2222 / btrfs subvol=@,compress=zstd:3,noatime 0 0\n"
		  "PARTUUID=2222 /home btrfs subvol=@home,compress=zstd:3,noatime 0 0\n"
		  "PARTUUID=1111 /boot vfat umask=0077,noatime 0 2\n"
		  "/dev/sdb1 /srv\\040data ext4 defaults 0 2\n");
	fsents_free(&f);
	/* an overmounted mount point: the last mount wins */
	const char *mi2 = "60 1 0:40 / /t rw - ext4 /dev/sda1 rw\n"
			  "61 60 0:41 / /t/boot rw - vfat /dev/sda2 rw\n"
			  "62 60 0:42 / /t/boot rw - vfat /dev/sda3 rw\n";
	cnt = mountinfo_fsents(mi2, "/t", NULL, NULL, &f);
	CHECK(cnt == 2 && f.n == 2 && !strcmp(f.v[1].spec, "/dev/sda3") && f.v[0].pass == 1, "overmount: %d", cnt);
	fsents_free(&f);
	check_str("unescape", mountinfo_unescape("a\\040b\\011c\\\\x"), "a b\tc\\\\x");

	/* /etc/shadow: replace the password and the change date, keep the other fields */
	check_str("shadow replace",
		  shadow_set("root:*:19000:0:99999:7:::\nbin:!:19000::::::\n", "root", "$y$abc", 20000),
		  "root:$y$abc:20000:0:99999:7:::\nbin:!:19000::::::\n");
	check_str("shadow user", shadow_set("root::1::::::\nanna:!:19000:0:99999:7:::\n", "anna", "$y$x", 5),
		  "root::1::::::\nanna:$y$x:5:0:99999:7:::\n");
	check_str("shadow append", shadow_set("root::1::::::", "bob", "$y$q", 7), "root::1::::::\nbob:$y$q:7:0:99999:7:::\n");
	check_str("shadow prefix name", shadow_set("rootx:!:1::::::\n", "root", "H", 2), "rootx:!:1::::::\nroot:H:2:0:99999:7:::\n");
	check_str("shadow short line", shadow_set("root:x\n", "root", "H", 3), "root:H:3:0:99999:7:::\n");
	check_str("groups none", user_groups("root:x:0:\nwheel:x:97:\n"), "wheel");
	check_str("groups all", user_groups("seat:x:950:\naudio:x:11:\nvideo:x:12:\nvideox:x:1:\n"), "wheel,audio,video,seat");
	check_str("groups render input", user_groups("input:x:97:\nrender:x:98:\n"), "wheel,render,input");
	check_str("groups prefix", user_groups("audiox:x:5:\nxvideo:x:6:\nseat"), "wheel");
	check_str("groups null", user_groups(NULL), "wheel");

	/* burn.conf: PROFILE replaced or appended, comments kept */
	check_str("conf replace", conf_set("# PROFILE=old\nPROFILE=native\nJOBS=4\n", "PROFILE", "steam-deck"),
		  "# PROFILE=old\nPROFILE=steam-deck\nJOBS=4\n");
	check_str("conf append", conf_set("JOBS=4\n", "PROFILE", "generic-x86_64"), "JOBS=4\nPROFILE=generic-x86_64\n");
	check_str("conf quotes", conf_set("", "FEATURES", "-docs +lto"), "FEATURES=\"-docs +lto\"\n");
	check_str("conf duplicates", conf_set("A=1\n  A=2\n", "A", "3"), "A=3\n");
	check_str("conf get", conf_get("# PROFILE=old\nPROFILE=native\n PROFILE=\"steam-deck\" # mine\n", "PROFILE"),
		  "steam-deck");
	check_str("conf get: unset", conf_get("JOBS=4\n", "PROFILE"), NULL);
	check_str("conf unset", conf_unset("# the image's\nFIRMWARE=\"amdgpu/ i915/\"\nJOBS=4\n FIRMWARE=x\n", "FIRMWARE"),
		  "# the image's\n# FIRMWARE=\"amdgpu/ i915/\"\nJOBS=4\n#  FIRMWARE=x\n");
	check_str("conf unset: nothing", conf_unset("JOBS=4", "FIRMWARE"), "JOBS=4\n");
	check_str("detect", detected_profile("arch       x86_64\nmodel      Jupiter\nprofile    steam-deck\n"),
		  "steam-deck");
	check_str("detect: none", detected_profile("arch x86_64\n"), NULL);

	/* the kernel command line */
	check_str("cmdline", cmdline_text("PARTUUID=abc", "subvol=@", "quiet"),
		  "root=PARTUUID=abc rootflags=subvol=@ rootwait ro quiet");
	check_str("cmdline plain", cmdline_text("PARTUUID=abc", NULL, NULL), "root=PARTUUID=abc rootwait ro");

	/* names */
	CHECK(valid_hostname("cleanlinux") && valid_hostname("box-1.lan") && !valid_hostname("-x") &&
		      !valid_hostname("a b") && !valid_hostname(""),
	      "hostnames");
	CHECK(valid_username("anna") && valid_username("_svc") && valid_username("a-1") && !valid_username("root") &&
		      !valid_username("Anna") && !valid_username("1x") && !valid_username(""),
	      "user names");
	check_str("size GiB", size_text(20ULL << 30), "20.0 GiB");
	check_str("size MiB", size_text(512ULL << 20), "512 MiB");

	/* time zones and console names (written into /etc/localtime and /etc/vconsole.conf) */
	CHECK(valid_zone("Europe/Moscow") && valid_zone("UTC") && valid_zone("America/Argentina/Buenos_Aires") &&
		      valid_zone("Etc/GMT+3") && valid_zone("America/Port-au-Prince"),
	      "zones");
	CHECK(!valid_zone("") && !valid_zone("/etc/passwd") && !valid_zone("../x") && !valid_zone("Europe/") &&
		      !valid_zone("Europe//Moscow") && !valid_zone("-x") && !valid_zone("a b") && !valid_zone("Europe/.."),
	      "bad zones");
	CHECK(!valid_zone("Europe/../../etc"), "zone with ..");
	CHECK(valid_console_name("us") && valid_console_name("ruwin_alt_sh-UTF-8") && valid_console_name("ter-v16n") &&
		      valid_console_name("latarcyrheb-sun16"),
	      "console names");
	CHECK(!valid_console_name("") && !valid_console_name("../us") && !valid_console_name("a/b") &&
		      !valid_console_name("-us") && !valid_console_name(".x") && !valid_console_name("a b"),
	      "bad console names");

	/* the progress of an installation from the installer's output */
	struct progress pr = {0};
	progress_line(&pr, "\033[1;36m::\033[0m Disk /dev/vdb (8.0 GiB)");
	CHECK(pr.step == STEP_DISK && pr.percent == 1, "progress disk %d %d", pr.step, pr.percent);
	progress_line(&pr, ":: Partitioning /dev/vdb");
	progress_line(&pr, ":: Formatting");
	CHECK(pr.step == STEP_FORMAT && pr.percent == 7, "progress format");
	progress_line(&pr, ":: Installing the packages");
	CHECK(pr.step == STEP_PACKAGES && pr.percent == 12, "progress packages");
	progress_line(&pr, "==> Packages (4): a b c d");
	CHECK(pr.pkgs == 4 && pr.pkgs_done == 0, "progress package count %d", pr.pkgs);
	progress_line(&pr, "==> Installing glibc 2.42-1");
	CHECK(pr.pkgs_done == 1 && !strcmp(pr.pkg, "glibc") && pr.percent == 12 + 76 / 4, "progress package 1: %d %s",
	      pr.percent, pr.pkg);
	progress_line(&pr, "\033[1m==> Installing linux 6.17.7-13\033[0m");
	CHECK(pr.pkgs_done == 2 && !strcmp(pr.pkg, "linux") && pr.percent == 12 + 76 * 2 / 4, "progress package 2: %s",
	      pr.pkg);
	progress_line(&pr, "some output of a hook");
	CHECK(pr.percent == 50 && pr.step == STEP_PACKAGES, "progress: other lines keep it");
	progress_line(&pr, "==> Installing x 1");
	progress_line(&pr, "==> Installing y 1");
	progress_line(&pr, "==> Installing z 1");
	CHECK(pr.pkgs_done == 5 && pr.percent == 88, "progress: never past the packages share: %d", pr.percent);
	progress_line(&pr, ":: Users");
	CHECK(pr.step == STEP_USERS && pr.percent == 92, "progress users");
	progress_line(&pr, ":: Partitioning");
	CHECK(pr.percent == 92, "progress never goes back");
	progress_line(&pr, ":: Done: CleanLinux is installed on /dev/vdb");
	CHECK(pr.step == STEP_DONE && pr.percent == 100, "progress done");
	/* the copy of the live system: the share of the bytes */
	struct progress pc = {0};
	progress_line(&pc, ":: Mounting");
	progress_line(&pc, "\033[1;36m::\033[0m Copying the system (2.4 GiB in 51234 files, nothing is built or downloaded)");
	CHECK(pc.step == STEP_PACKAGES && pc.percent == 12 && !pc.copied && !pc.pkgs, "progress copy %d", pc.percent);
	progress_line(&pc, "==> Copied 50% (1.2 GiB of 2.4 GiB)");
	CHECK(pc.copied == 50 && pc.percent == 12 + 38 && !strcmp(pc.pkg, "1.2 GiB of 2.4 GiB"), "progress copied %d %s",
	      pc.percent, pc.pkg);
	progress_line(&pc, "==> Copied 100% (2.4 GiB of 2.4 GiB)");
	CHECK(pc.copied == 100 && pc.percent == 88, "progress copied all %d", pc.percent);
	progress_line(&pc, "==> Copied 300% (x)");
	CHECK(pc.copied == 100 && pc.percent == 88, "progress copied: nonsense ignored");
	progress_line(&pc, ":: Configuring the system");
	CHECK(pc.step == STEP_CONFIG && pc.percent == 89, "progress after the copy");

	/* UTF-8 and wrapping (columns = code points) */
	const char *u = "aЖ€😀";
	const char *up = u;
	CHECK(utf8_next(&up) == 'a' && utf8_next(&up) == 0x416 && utf8_next(&up) == 0x20ac && utf8_next(&up) == 0x1f600 &&
		      !*up,
	      "utf8_next");
	const char *bad = "\xff" "b";
	CHECK(utf8_next(&bad) == 0xfffd && *bad == 'b', "utf8_next: a bad byte");
	CHECK(utf8_len(u) == 4 && utf8_len("") == 0, "utf8_len");
	CHECK(utf8_prefix(u, 2) == 3 && utf8_prefix(u, 10) == strlen(u), "utf8_prefix");
	char ub[4];
	CHECK(utf8_put(0x416, ub) == 2 && (unsigned char)ub[0] == 0xd0 && (unsigned char)ub[1] == 0x96, "utf8_put");
	check_str("wrap", text_wrap("Все данные на диске будут удалены", 12), "Все данные\nна диске\nбудут\nудалены");
	check_str("wrap paragraphs", text_wrap("one two\n\nthree", 20), "one two\n\nthree");
	check_str("wrap long word", text_wrap("abcdefghij", 4), "abcd\nefgh\nij");
	check_str("wrap spaces", text_wrap("  a   b  ", 10), "  a   b");
	check_str("wrap table", text_wrap("Disk:     /dev/vdb  20 GiB\nUser:     anna", 40),
		  "Disk:     /dev/vdb  20 GiB\nUser:     anna");
	check_str("wrap at spaces", text_wrap("aaaa    bbbb cc", 8), "aaaa\nbbbb cc");

	test_tree_copy();

	printf("unit: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
