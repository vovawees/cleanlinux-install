/* SPDX-License-Identifier: MIT */
/* What touches the machine: disks in sysfs, libblkid, btrfs ioctls, EFI variables, passwords. */
#define _GNU_SOURCE
#include "install.h"

#include <blkid/blkid.h>
#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/btrfs.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

uint64_t sysfs_u64(const char *dir, const char *file)
{
	char *p = xasprintf("%s/%s", dir, file);
	char *s = read_line(p);
	free(p);
	uint64_t v = s ? strtoull(s, NULL, 10) : 0;
	free(s);
	return v;
}

static char *sysfs_str(const char *dir, const char *file)
{
	char *p = xasprintf("%s/%s", dir, file);
	char *s = read_line(p);
	free(p);
	return s ? s : xstrdup("");
}

/* the sysfs directory of a block device node, or NULL */
static char *sysfs_of(const char *dev)
{
	struct stat st;
	if (stat(dev, &st) < 0 || !S_ISBLK(st.st_mode))
		return NULL;
	char *link = xasprintf("/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
	char *real = realpath(link, NULL);
	free(link);
	return real;
}

void disk_open(const char *path, struct disk *d, bool check_busy)
{
	struct stat st;
	if (stat(path, &st) < 0)
		die_errno("%s", path);
	if (!S_ISBLK(st.st_mode))
		die("%s is not a block device", path);
	char *sys = sysfs_of(path);
	if (!sys)
		die("%s: not found in /sys/dev/block", path);
	char *part = xasprintf("%s/partition", sys);
	if (exists(part)) {
		char *parent = xstrdup(sys);
		*strrchr(parent, '/') = '\0';
		die("%s is a partition: give the whole disk (/dev/%s)", path, strrchr(parent, '/') + 1);
	}
	free(part);
	d->sys = sys;
	d->name = xstrdup(strrchr(sys, '/') + 1);
	d->dev = xasprintf("/dev/%s", d->name);
	d->bytes = sysfs_u64(sys, "size") * 512;
	d->lbs = (unsigned)sysfs_u64(sys, "queue/logical_block_size");
	if (!d->lbs)
		d->lbs = 512;
	char *vendor = sysfs_str(sys, "device/vendor"), *model = sysfs_str(sys, "device/model");
	if (str_prefix(vendor, "0x")) /* virtio: a PCI vendor number, no name */
		*vendor = '\0';
	d->model = *vendor && *model ? xasprintf("%s %s", vendor, model) : xstrdup(*model ? model : vendor);
	free(vendor);
	free(model);
	d->removable = sysfs_u64(sys, "removable") != 0;
	if (sysfs_u64(sys, "ro"))
		die("%s is read-only", d->dev);
	if (!d->bytes)
		die("%s has no medium (size 0)", d->dev);
	if (check_busy) {
		/* the kernel refuses an exclusive open while a partition is mounted, a swap, or held by
		 * device mapper or md: the disk of the running system cannot be chosen by mistake */
		int fd = open(d->dev, O_RDONLY | O_EXCL | O_CLOEXEC);
		if (fd < 0) {
			if (errno == EBUSY)
				die("%s is in use: a partition is mounted, a swap, or held by device mapper or RAID", d->dev);
			die_errno("%s", d->dev);
		}
		close(fd);
	}
}

/* the partitions of the disk: "name size" lines for the confirmation */
void disk_describe(const struct disk *d)
{
	char *size = size_text(d->bytes);
	info("%s: %s%s%s%s", d->dev, size, *d->model ? ", " : "", d->model, d->removable ? ", removable" : "");
	free(size);
	DIR *dh = opendir(d->sys);
	if (!dh)
		return;
	struct dirent *e;
	strv parts = STRV_INIT;
	while ((e = readdir(dh)))
		if (str_prefix(e->d_name, d->name)) {
			char *dir = xasprintf("%s/%s", d->sys, e->d_name);
			char *pf = xasprintf("%s/partition", dir);
			if (exists(pf)) {
				char *dev = xasprintf("/dev/%s", e->d_name);
				char *ps = size_text(sysfs_u64(dir, "size") * 512);
				char *type = fstype_of(dev);
				strv_take(&parts, xasprintf("%s: %s%s%s", dev, ps, type ? ", " : "", type ? type : ""));
				free(type);
				free(ps);
				free(dev);
			}
			free(pf);
			free(dir);
		}
	closedir(dh);
	for (size_t i = 0; i < parts.n; i++)
		info("  %s", parts.v[i]);
	strv_free(&parts);
}

/* the sysfs directory of partition N of the disk, or NULL */
static char *part_sys(const struct disk *d, unsigned n)
{
	DIR *dh = opendir(d->sys);
	if (!dh)
		return NULL;
	struct dirent *e;
	char *found = NULL;
	while (!found && (e = readdir(dh))) {
		if (!str_prefix(e->d_name, d->name))
			continue;
		char *dir = xasprintf("%s/%s", d->sys, e->d_name);
		char *pf = xasprintf("%s/partition", dir);
		if (exists(pf) && sysfs_u64(dir, "partition") == n)
			found = dir;
		else
			free(dir);
		free(pf);
	}
	closedir(dh);
	return found;
}

char *disk_part_dev(const struct disk *d, unsigned n, int wait_ms)
{
	if (wait_ms <= 0) {
		char *name = part_name(d->name, n);
		char *dev = xasprintf("/dev/%s", name);
		free(name);
		return dev;
	}
	for (int waited = 0; waited <= wait_ms; waited += 100) {
		char *dir = part_sys(d, n);
		if (dir) {
			char *dev = xasprintf("/dev/%s", strrchr(dir, '/') + 1);
			free(dir);
			if (exists(dev))
				return dev;
			free(dev);
		}
		struct timespec ts = {0, 100 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	return NULL;
}

static char *blkid_value(const char *dev, const char *key, bool partitions)
{
	blkid_probe pr = blkid_new_probe_from_filename(dev);
	if (!pr)
		return NULL;
	char *out = NULL;
	if (partitions) {
		blkid_probe_enable_superblocks(pr, 0);
		blkid_probe_enable_partitions(pr, 1);
		blkid_probe_set_partitions_flags(pr, BLKID_PARTS_ENTRY_DETAILS);
	} else {
		blkid_probe_enable_superblocks(pr, 1);
		blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_UUID);
	}
	const char *v;
	if (blkid_do_safeprobe(pr) == 0 && blkid_probe_lookup_value(pr, key, &v, NULL) == 0 && v && *v)
		out = xstrdup(v);
	blkid_free_probe(pr);
	return out;
}

char *partuuid_of(const char *dev, void *ctx)
{
	char *u = blkid_value(dev, "PART_ENTRY_UUID", true);
	if (!u)
		return NULL;
	char *s = xasprintf("PARTUUID=%s", u);
	free(u);
	return s;
}

char *fstype_of(const char *dev) { return blkid_value(dev, "TYPE", false); }

int btrfs_subvol_create(const char *parent, const char *name)
{
	int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct btrfs_ioctl_vol_args args;
	memset(&args, 0, sizeof(args));
	snprintf(args.name, sizeof(args.name), "%s", name);
	int r = ioctl(fd, BTRFS_IOC_SUBVOL_CREATE, &args);
	int e = errno;
	close(fd);
	errno = e;
	return r;
}

#define EFIVARS "/sys/firmware/efi/efivars"
#define EFI_GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"

/* write an EFI global variable (non-volatile, boot and runtime access): one write() */
static int efivar_write(const char *name, const uint8_t *data, size_t len)
{
	char *p = xasprintf(EFIVARS "/%s-" EFI_GLOBAL, name);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		/* efivarfs marks some variables immutable */
		int fl;
		if (ioctl(fd, FS_IOC_GETFLAGS, &fl) == 0 && (fl & FS_IMMUTABLE_FL)) {
			fl &= ~FS_IMMUTABLE_FL;
			ioctl(fd, FS_IOC_SETFLAGS, &fl);
		}
		close(fd);
	}
	fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	free(p);
	if (fd < 0)
		return -1;
	uint8_t *buf = xmalloc(len + 4);
	uint32_t attrs = 7; /* NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */
	memcpy(buf, &attrs, 4);
	memcpy(buf + 4, data, len);
	ssize_t w = write(fd, buf, len + 4);
	int e = errno;
	free(buf);
	close(fd);
	if (w != (ssize_t)(len + 4)) {
		errno = w < 0 ? e : EIO;
		return -1;
	}
	return 0;
}

/* the value of an EFI global variable without its attributes */
static uint8_t *efivar_read(const char *name, size_t *len)
{
	char *p = xasprintf(EFIVARS "/%s-" EFI_GLOBAL, name);
	size_t n;
	char *s = read_file(p, &n);
	free(p);
	if (!s || n < 4) {
		free(s);
		return NULL;
	}
	memmove(s, s + 4, n - 4);
	*len = n - 4;
	return (uint8_t *)s;
}

int efi_boot_entry(const struct disk *d, unsigned partno, const char *partuuid, const char *loader, const char *desc)
{
	if (!is_dir(EFIVARS)) {
		errno = ENOENT;
		return -1;
	}
	char *psys = part_sys(d, partno);
	if (!psys) {
		errno = ENOENT;
		return -1;
	}
	uint64_t start = sysfs_u64(psys, "start") * 512 / d->lbs, size = sysfs_u64(psys, "size") * 512 / d->lbs;
	free(psys);
	uint8_t guid[16], opt[1024];
	if (guid_bytes(partuuid, guid) < 0) {
		errno = EINVAL;
		return -1;
	}
	size_t len = efi_load_option(opt, sizeof(opt), desc, partno, start, size, guid, loader);
	if (!len) {
		errno = E2BIG;
		return -1;
	}
	/* the same entry again (a reinstall) keeps its number; otherwise the first free one */
	int num = -1;
	static bool used[0x10000];
	memset(used, 0, sizeof(used));
	DIR *dh = opendir(EFIVARS);
	if (dh) {
		struct dirent *e;
		while ((e = readdir(dh))) {
			unsigned n;
			char rest[64];
			if (strlen(e->d_name) != 8 + 1 + 36 || sscanf(e->d_name, "Boot%4X-%63s", &n, rest) != 2 ||
			    strcmp(rest, EFI_GLOBAL) || !isxdigit((unsigned char)e->d_name[7]))
				continue;
			used[n] = true;
			char name[16];
			snprintf(name, sizeof(name), "Boot%04X", n);
			size_t l;
			uint8_t *v = efivar_read(name, &l);
			if (v && l == len && !memcmp(v, opt, len) && num < 0)
				num = (int)n;
			free(v);
		}
		closedir(dh);
	}
	for (int n = 0; num < 0 && n < 0x10000; n++)
		if (!used[n])
			num = n;
	char name[16];
	snprintf(name, sizeof(name), "Boot%04X", (unsigned)num);
	if (efivar_write(name, opt, len) < 0)
		return -1;
	/* first in BootOrder */
	size_t ol = 0;
	uint8_t *old = efivar_read("BootOrder", &ol);
	uint8_t *order = xmalloc(ol + 2);
	size_t n = 0;
	order[n++] = (uint8_t)num;
	order[n++] = (uint8_t)(num >> 8);
	for (size_t i = 0; old && i + 1 < ol; i += 2)
		if ((old[i] | old[i + 1] << 8) != num) {
			order[n++] = old[i];
			order[n++] = old[i + 1];
		}
	int r = efivar_write("BootOrder", order, n);
	free(order);
	free(old);
	if (r == 0)
		info("EFI boot entry %s: %s (first in BootOrder)", name, desc);
	return r;
}

char *hash_password(const char *pw)
{
	/* the library's default method (yescrypt), a random salt from the kernel */
	char *salt = crypt_gensalt_ra(NULL, 0, NULL, 0);
	if (!salt)
		die_errno("crypt_gensalt");
	struct crypt_data *cd = calloc(1, sizeof(*cd));
	if (!cd)
		die("out of memory");
	char *h = crypt_r(pw, salt, cd);
	if (!h || *h == '*')
		die("crypt failed");
	char *out = xstrdup(h);
	explicit_bzero(cd, sizeof(*cd));
	free(cd);
	free(salt);
	return out;
}

static char *tty_line(int fd, bool echo)
{
	struct termios old, t;
	bool restore = false;
	if (!echo && tcgetattr(fd, &old) == 0) {
		t = old;
		t.c_lflag &= ~(tcflag_t)ECHO;
		t.c_lflag |= ECHONL;
		restore = tcsetattr(fd, TCSAFLUSH, &t) == 0;
	}
	char buf[512];
	size_t n = 0;
	for (;;) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0 || c == '\n')
			break;
		if (n + 1 < sizeof(buf))
			buf[n++] = c;
	}
	buf[n] = '\0';
	if (restore)
		tcsetattr(fd, TCSAFLUSH, &old);
	char *s = xstrdup(buf);
	explicit_bzero(buf, sizeof(buf));
	return s;
}

char *ask_password(const char *who)
{
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		die("no terminal to ask the password of %s: use --%s-password-file", who,
		    strcmp(who, "root") ? "user" : "root");
	for (int tries = 0; tries < 3; tries++) {
		dprintf(fd, "New password for %s: ", who);
		char *a = tty_line(fd, false);
		if (!*a) {
			dprintf(fd, "The password is empty, try again.\n");
			free(a);
			continue;
		}
		dprintf(fd, "Retype the password for %s: ", who);
		char *b = tty_line(fd, false);
		bool same = !strcmp(a, b);
		explicit_bzero(b, strlen(b));
		free(b);
		if (same) {
			close(fd);
			return a;
		}
		explicit_bzero(a, strlen(a));
		free(a);
		dprintf(fd, "The passwords differ, try again.\n");
	}
	die("no password for %s", who);
}

char *read_password_file(const char *path)
{
	char *s = strcmp(path, "-") ? read_file(path, NULL) : read_file("/dev/stdin", NULL);
	if (!s)
		die_errno("%s", path);
	s[strcspn(s, "\r\n")] = '\0';
	if (!*s)
		die("%s: the first line (the password) is empty", path);
	return s;
}

bool ask_yes(const char *question)
{
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		die("no terminal to confirm: use --yes");
	dprintf(fd, "%s ", question);
	char *a = tty_line(fd, true);
	close(fd);
	bool yes = !strcmp(trim(a), "yes");
	free(a);
	return yes;
}
