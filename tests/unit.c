/* SPDX-License-Identifier: MIT */
/* Unit tests of the pure parts of the installer (src/plan.c). */
#define _GNU_SOURCE
#include "install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	fsents_add(&f, "/dev/mapper/luks-u-1", "/", "btrfs", "subvol=@", 0);
	check_str("fstab luks", fstab_text(&f),
		  "# /etc/fstab, written by cleanlinux-install. Partitions by PARTUUID: the names survive\n"
		  "# reformatting and disk order (ls -l /dev/disk/by-partuuid).\n"
		  "# <device> <mount point> <type> <options> <dump> <pass>\n"
		  "# The encrypted root: /dev/mapper/luks-<UUID of the LUKS container>, opened by the\n"
		  "# initramfs (rd.luks.uuid= on the kernel command line).\n"
		  "/dev/mapper/luks-u-1 / btrfs subvol=@ 0 0\n");
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
	check_str("detect", detected_profile("arch       x86_64\nmodel      Jupiter\nprofile    steam-deck\n"),
		  "steam-deck");
	check_str("detect: none", detected_profile("arch x86_64\n"), NULL);

	/* the kernel command line */
	check_str("cmdline", cmdline_text("PARTUUID=abc", NULL, "subvol=@", "quiet"),
		  "root=PARTUUID=abc rootflags=subvol=@ rootwait ro quiet");
	check_str("cmdline plain", cmdline_text("PARTUUID=abc", NULL, NULL, NULL), "root=PARTUUID=abc rootwait ro");
	check_str("cmdline luks", cmdline_text("/dev/mapper/luks-u-1", "u-1", "subvol=@", NULL),
		  "rd.luks.uuid=u-1 root=/dev/mapper/luks-u-1 rootflags=subvol=@ rootwait ro");

	/* LUKS containers by their device-mapper UUID */
	check_str("luks2 dm uuid", luks_uuid_of_dm("CRYPT-LUKS2-7946c1de0d284951b5437ba98661c25c-cryptroot"),
		  "7946c1de-0d28-4951-b543-7ba98661c25c");
	check_str("luks1 dm uuid", luks_uuid_of_dm("CRYPT-LUKS1-7946C1DE0D284951B5437BA98661C25C-x\n"),
		  "7946c1de-0d28-4951-b543-7ba98661c25c");
	check_str("plain dm-crypt", luks_uuid_of_dm("CRYPT-PLAIN-cryptswap"), NULL);
	check_str("lvm", luks_uuid_of_dm("LVM-abcdefabcdefabcdefabcdefabcdefab"), NULL);
	check_str("short", luks_uuid_of_dm("CRYPT-LUKS2-7946c1de-x"), NULL);
	check_str("mapper", luks_mapper("u-1"), "/dev/mapper/luks-u-1");

	/* names */
	CHECK(valid_hostname("cleanlinux") && valid_hostname("box-1.lan") && !valid_hostname("-x") &&
		      !valid_hostname("a b") && !valid_hostname(""),
	      "hostnames");
	CHECK(valid_username("anna") && valid_username("_svc") && valid_username("a-1") && !valid_username("root") &&
		      !valid_username("Anna") && !valid_username("1x") && !valid_username(""),
	      "user names");
	check_str("size GiB", size_text(20ULL << 30), "20.0 GiB");
	check_str("size MiB", size_text(512ULL << 20), "512 MiB");

	printf("unit: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
