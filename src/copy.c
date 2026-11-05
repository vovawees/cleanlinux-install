/* SPDX-License-Identifier: MIT */
/* The system copied onto the new disk: the tree of the live medium (@/) and the layers over it
 * (nonfree/, spins/NAME/), one after the other, as the overlay of the live system shows them.
 * Everything is kept that a package manager or a running system notices: owners, modes, times,
 * extended attributes (file capabilities, ACLs), hard links, device nodes and FIFOs; sockets are
 * left out, and so are filesystems mounted below the source. On a vfat directory of the target
 * (the ESP at /boot) only the data and the times are written. */
#define _GNU_SOURCE
#include "install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#define COPY_BUF (1 << 20)

struct hlink {
	dev_t dev;
	ino_t ino;
	char *path; /* the first copy in the target */
};

struct copier {
	tree_progress cb;
	void *ctx;
	uint64_t bytes, entries, reported;
	struct hlink *links;
	size_t nlinks, caplinks;
	char *buf;
	int errors;
	dev_t src_dev;
};

static void fail(struct copier *c, const char *what, const char *path)
{
	warn_errno("%s %s", what, path);
	c->errors++;
}

static void report(struct copier *c, bool force)
{
	if (c->cb && (force || c->bytes - c->reported >= (8u << 20))) {
		c->reported = c->bytes;
		c->cb(c->bytes, c->entries, c->ctx);
	}
}

/* ---- measuring ---- */

static void measure(const char *path, dev_t dev, struct tree_size *t)
{
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = xasprintf("%s/%s", path, e->d_name);
		struct stat st;
		if (lstat(p, &st) == 0) {
			t->entries++;
			if (S_ISREG(st.st_mode))
				t->bytes += (uint64_t)st.st_size;
			else if (S_ISDIR(st.st_mode) && st.st_dev == dev)
				measure(p, dev, t);
		}
		free(p);
	}
	closedir(d);
}

void tree_measure(const char *src, struct tree_size *t)
{
	*t = (struct tree_size){0, 0};
	struct stat st;
	if (lstat(src, &st) == 0 && S_ISDIR(st.st_mode))
		measure(src, st.st_dev, t);
}

/* ---- removing what a layer replaces ---- */

static int remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	DIR *d = opendir(path);
	if (!d)
		return -1;
	struct dirent *e;
	int r = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = xasprintf("%s/%s", path, e->d_name);
		if (remove_tree(p) < 0)
			r = -1;
		free(p);
	}
	closedir(d);
	return r < 0 ? r : rmdir(path);
}

/* the contents of the directory PATH, which stays */
static int empty_dir(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return -1;
	struct dirent *e;
	int r = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = xasprintf("%s/%s", path, e->d_name);
		if (remove_tree(p) < 0)
			r = -1;
		free(p);
	}
	closedir(d);
	return r;
}

/* ---- metadata ---- */

static bool overlay_attr(const char *name)
{
	return str_prefix(name, "trusted.overlay.") || str_prefix(name, "user.overlay.");
}

static bool is_opaque(const char *path)
{
	char v[2];
	return (lgetxattr(path, "trusted.overlay.opaque", v, sizeof(v)) == 1 && v[0] == 'y') ||
	       (lgetxattr(path, "user.overlay.opaque", v, sizeof(v)) == 1 && v[0] == 'y');
}

static void copy_xattrs(struct copier *c, const char *src, const char *dst)
{
	ssize_t n = llistxattr(src, NULL, 0);
	if (n <= 0)
		return;
	char *names = xmalloc((size_t)n);
	n = llistxattr(src, names, (size_t)n);
	for (ssize_t i = 0; n > 0 && i < n; i += (ssize_t)strlen(names + i) + 1) {
		const char *name = names + i;
		if (overlay_attr(name))
			continue;
		ssize_t vl = lgetxattr(src, name, NULL, 0);
		if (vl < 0)
			continue;
		char *v = xmalloc((size_t)vl + 1);
		vl = lgetxattr(src, name, v, (size_t)vl);
		if (vl >= 0 && lsetxattr(dst, name, v, (size_t)vl, 0) < 0 && errno != ENOTSUP && errno != EOPNOTSUPP &&
		    errno != EPERM) {
			warn_errno("extended attribute %s of %s", name, dst);
			c->errors++;
		}
		free(v);
	}
	free(names);
}

/* owner, mode, extended attributes and times of DST as of ST */
static void copy_meta(struct copier *c, const char *src, const char *dst, const struct stat *st, bool vfat)
{
	bool link = S_ISLNK(st->st_mode);
	if (!vfat) {
		if (lchown(dst, st->st_uid, st->st_gid) < 0 && !(errno == EPERM && geteuid() != 0))
			fail(c, "chown", dst);
		if (!link && chmod(dst, st->st_mode & 07777) < 0)
			fail(c, "chmod", dst);
		copy_xattrs(c, src, dst);
	}
	struct timespec ts[2] = {st->st_atim, st->st_mtim};
	if (utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW) < 0 && !(vfat && link))
		fail(c, "times of", dst);
}

/* ---- the copy ---- */

static int copy_data(struct copier *c, const char *src, const char *dst, const struct stat *st)
{
	int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (in < 0)
		return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 0) {
		int e = errno;
		close(in);
		errno = e;
		return -1;
	}
	bool cfr = true;
	int r = 0;
	for (off_t left = st->st_size; left > 0;) {
		size_t want = left > (off_t)(1 << 30) ? (size_t)1 << 30 : (size_t)left;
		ssize_t n;
		if (cfr) {
			n = copy_file_range(in, NULL, out, NULL, want, 0);
			if ((n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP ||
				       errno == EBADF || errno == EPERM)) ||
			    n == 0) {
				cfr = false; /* not between these filesystems: read and write */
				continue;
			}
		} else {
			n = read(in, c->buf, want < COPY_BUF ? want : COPY_BUF);
			for (ssize_t w = 0; n > 0 && w < n;) {
				ssize_t k = write(out, c->buf + w, (size_t)(n - w));
				if (k < 0 && errno == EINTR)
					continue;
				if (k <= 0) {
					n = -1;
					break;
				}
				w += k;
			}
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			r = -1;
			break;
		}
		if (n == 0)
			break; /* it got shorter */
		left -= n;
		c->bytes += (uint64_t)n;
		report(c, false);
	}
	int e = errno;
	if (close(out) < 0 && r == 0) {
		e = errno;
		r = -1;
	}
	close(in);
	errno = e;
	return r;
}

static const char *find_link(struct copier *c, const struct stat *st)
{
	for (size_t i = 0; i < c->nlinks; i++)
		if (c->links[i].dev == st->st_dev && c->links[i].ino == st->st_ino)
			return c->links[i].path;
	return NULL;
}

static void add_link(struct copier *c, const struct stat *st, const char *dst)
{
	if (c->nlinks == c->caplinks) {
		c->caplinks = c->caplinks ? c->caplinks * 2 : 64;
		c->links = xrealloc(c->links, c->caplinks * sizeof(*c->links));
	}
	c->links[c->nlinks++] = (struct hlink){st->st_dev, st->st_ino, xstrdup(dst)};
}

static void copy_dir(struct copier *c, const char *src, const char *dst);

static void copy_entry(struct copier *c, const char *src, const char *dst, bool vfat)
{
	struct stat st, dt;
	if (lstat(src, &st) < 0) {
		fail(c, "stat", src);
		return;
	}
	c->entries++;
	bool there = lstat(dst, &dt) == 0;
	/* a whiteout of a layer: what is below is gone */
	if (S_ISCHR(st.st_mode) && st.st_rdev == makedev(0, 0)) {
		if (there && remove_tree(dst) < 0)
			fail(c, "remove", dst);
		return;
	}
	if (S_ISSOCK(st.st_mode))
		return;
	if (S_ISDIR(st.st_mode)) {
		if (there && !S_ISDIR(dt.st_mode)) {
			if (unlink(dst) < 0) {
				fail(c, "remove", dst);
				return;
			}
			there = false;
		}
		if (!there && mkdir(dst, 0700) < 0) {
			fail(c, "mkdir", dst);
			return;
		}
		if (there && is_opaque(src) && empty_dir(dst) < 0)
			fail(c, "empty", dst);
		if (st.st_dev == c->src_dev)
			copy_dir(c, src, dst);
		struct statfs sf;
		bool dvfat = statfs(dst, &sf) == 0 && sf.f_type == MSDOS_SUPER_MAGIC;
		/* a mount point of the target keeps its own owner and mode (the ESP, @home) */
		if (!(there && is_mountpoint(dst)))
			copy_meta(c, src, dst, &st, dvfat);
		return;
	}
	if (there && remove_tree(dst) < 0) {
		fail(c, "replace", dst);
		return;
	}
	if (S_ISREG(st.st_mode)) {
		const char *first = st.st_nlink > 1 && !vfat ? find_link(c, &st) : NULL;
		if (first) {
			if (link(first, dst) < 0)
				fail(c, "link", dst);
			return;
		}
		if (copy_data(c, src, dst, &st) < 0) {
			fail(c, "copy", src);
			return;
		}
		if (st.st_nlink > 1 && !vfat)
			add_link(c, &st, dst);
	} else if (S_ISLNK(st.st_mode)) {
		if (vfat)
			return; /* vfat has no symbolic links */
		char *target = xmalloc((size_t)st.st_size + 2);
		ssize_t n = readlink(src, target, (size_t)st.st_size + 1);
		if (n < 0 || n > st.st_size) {
			free(target);
			fail(c, "readlink", src);
			return;
		}
		target[n] = '\0';
		int r = symlink(target, dst);
		free(target);
		if (r < 0) {
			fail(c, "symlink", dst);
			return;
		}
	} else {
		if (vfat)
			return;
		if (mknod(dst, st.st_mode & (S_IFMT | 07777), st.st_rdev) < 0) {
			fail(c, "mknod", dst);
			return;
		}
	}
	copy_meta(c, src, dst, &st, vfat);
}

static void copy_dir(struct copier *c, const char *src, const char *dst)
{
	DIR *d = opendir(src);
	if (!d) {
		fail(c, "open", src);
		return;
	}
	struct statfs sf;
	bool vfat = statfs(dst, &sf) == 0 && sf.f_type == MSDOS_SUPER_MAGIC;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *s = xasprintf("%s/%s", src, e->d_name), *t = xasprintf("%s/%s", dst, e->d_name);
		copy_entry(c, s, t, vfat);
		free(s);
		free(t);
	}
	closedir(d);
}

int tree_copy(const char *src, const char *dst, tree_progress cb, void *ctx)
{
	struct copier c = {.cb = cb, .ctx = ctx, .buf = xmalloc(COPY_BUF)};
	struct stat st;
	if (lstat(src, &st) < 0 || !S_ISDIR(st.st_mode)) {
		warn_errno("%s: not a directory", src);
		free(c.buf);
		return 1;
	}
	c.src_dev = st.st_dev;
	if (mkdir_p(dst, 0755) < 0) {
		warn_errno("mkdir %s", dst);
		free(c.buf);
		return 1;
	}
	copy_dir(&c, src, dst);
	struct statfs sf;
	bool vfat = statfs(dst, &sf) == 0 && sf.f_type == MSDOS_SUPER_MAGIC;
	if (!is_mountpoint(dst) || !vfat)
		copy_meta(&c, src, dst, &st, vfat);
	report(&c, true);
	for (size_t i = 0; i < c.nlinks; i++)
		free(c.links[i].path);
	free(c.links);
	free(c.buf);
	return c.errors;
}
