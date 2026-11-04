/* SPDX-License-Identifier: MIT */
/* The pure parts of the installer: texts it writes and the layouts it computes. No I/O here
 * except random numbers, so tests/unit.c checks all of it. */
#define _GNU_SOURCE
#include "install.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fsents_add(fsents *f, const char *spec, const char *dir, const char *type, const char *opts, int pass)
{
	if (f->n == f->cap) {
		f->cap = f->cap ? f->cap * 2 : 8;
		f->v = xrealloc(f->v, f->cap * sizeof(*f->v));
	}
	f->v[f->n++] = (struct fsent){xstrdup(spec), xstrdup(dir), xstrdup(type), xstrdup(opts), pass};
}

void fsents_free(fsents *f)
{
	for (size_t i = 0; i < f->n; i++) {
		free(f->v[i].spec);
		free(f->v[i].dir);
		free(f->v[i].type);
		free(f->v[i].opts);
	}
	free(f->v);
	*f = (fsents){0};
}

/* fstab fields: blanks and backslashes as octal escapes */
static void fstab_field(sbuf *b, const char *s)
{
	for (; *s; s++)
		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\')
			sb_printf(b, "\\%03o", (unsigned char)*s);
		else
			sb_addn(b, s, 1);
}

char *fstab_text(const fsents *f)
{
	sbuf b = SBUF_INIT;
	sb_add(&b, "# /etc/fstab, written by cleanlinux-install. Partitions by PARTUUID: the names survive\n"
		   "# reformatting and disk order (ls -l /dev/disk/by-partuuid).\n"
		   "# <device> <mount point> <type> <options> <dump> <pass>\n");
	for (size_t i = 0; i < f->n; i++) {
		fstab_field(&b, f->v[i].spec);
		sb_add(&b, " ");
		fstab_field(&b, f->v[i].dir);
		sb_printf(&b, " %s %s 0 %d\n", f->v[i].type, f->v[i].opts, f->v[i].pass);
	}
	return sb_take(&b);
}

char *mountinfo_unescape(const char *s)
{
	char *out = xmalloc(strlen(s) + 1), *o = out;
	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*o++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
			s += 4;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
	return out;
}

/* the value of KEY= in a comma separated option list (a copy), NULL when absent */
static char *opt_value(const char *opts, const char *key)
{
	size_t kl = strlen(key);
	for (const char *p = opts; p && *p;) {
		size_t len = strcspn(p, ",");
		if (len > kl && !strncmp(p, key, kl) && p[kl] == '=')
			return xstrndup(p + kl + 1, len - kl - 1);
		p += len;
		if (*p == ',')
			p++;
	}
	return NULL;
}

static bool opt_has(const char *opts, const char *word)
{
	size_t wl = strlen(word);
	for (const char *p = opts; p && *p;) {
		size_t len = strcspn(p, ",");
		if (len == wl && !strncmp(p, word, wl))
			return true;
		p += len;
		if (*p == ',')
			p++;
	}
	return false;
}

static void opt_add(sbuf *b, const char *opt)
{
	if (b->n)
		sb_add(b, ",");
	sb_add(b, opt);
}

int mountinfo_fsents(const char *mountinfo, const char *target, spec_fn spec, void *ctx, fsents *out)
{
	size_t tl = strlen(target);
	while (tl > 1 && target[tl - 1] == '/')
		tl--;
	int count = 0;
	char *copy = xstrdup(mountinfo);
	for (char *save = NULL, *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		/* id parent major:minor root mountpoint options [optional fields] - type source super-options */
		strv f = STRV_INIT;
		strv_split(&f, line);
		size_t dash = 0;
		for (size_t i = 6; i < f.n; i++)
			if (!strcmp(f.v[i], "-")) {
				dash = i;
				break;
			}
		if (!dash || dash + 2 >= f.n) {
			strv_free(&f);
			continue;
		}
		char *mp = mountinfo_unescape(f.v[4]), *src = mountinfo_unescape(f.v[dash + 2]);
		const char *type = f.v[dash + 1], *mopts = f.v[5], *sopts = dash + 3 < f.n ? f.v[dash + 3] : "";
		const char *rel = NULL;
		if (!strncmp(mp, target, tl) && (mp[tl] == '\0' || mp[tl] == '/'))
			rel = tl == 1 ? mp : mp[tl] ? mp + tl : "/";
		if (rel && str_prefix(src, "/dev/")) {
			sbuf o = SBUF_INIT;
			int pass = 0;
			if (!strcmp(type, "btrfs")) {
				char *sub = opt_value(sopts, "subvol"), *comp = opt_value(sopts, "compress"),
				     *force = opt_value(sopts, "compress-force");
				if (sub && strcmp(sub, "/")) {
					char *s = xasprintf("subvol=%s", sub + (*sub == '/'));
					opt_add(&o, s);
					free(s);
				}
				if (force || comp) {
					char *s = force ? xasprintf("compress-force=%s", force) : xasprintf("compress=%s", comp);
					opt_add(&o, s);
					free(s);
				}
				free(sub);
				free(comp);
				free(force);
				if (opt_has(mopts, "noatime"))
					opt_add(&o, "noatime");
			} else if (!strcmp(type, "vfat") &&
				   (!strcmp(rel, "/boot") || !strcmp(rel, "/efi") || !strcmp(rel, "/boot/efi"))) {
				opt_add(&o, ESP_OPTS);
				pass = 2;
			} else {
				if (opt_has(mopts, "noatime"))
					opt_add(&o, "noatime");
				if (!strcmp(type, "vfat"))
					pass = 2;
				else if (!strcmp(type, "ext4") || !strcmp(type, "ext3") || !strcmp(type, "ext2"))
					pass = !strcmp(rel, "/") ? 1 : 2;
			}
			if (!o.n)
				sb_add(&o, "defaults");
			char *sp = spec ? spec(src, ctx) : NULL;
			char *opts = sb_take(&o);
			/* a mount point mounted twice: the last one wins, as in the kernel */
			for (size_t i = 0; i < out->n; i++)
				if (!strcmp(out->v[i].dir, rel)) {
					free(out->v[i].dir);
					out->v[i].dir = xstrdup("");
				}
			fsents_add(out, sp ? sp : src, rel, type, opts, pass);
			free(opts);
			free(sp);
			count++;
		}
		free(mp);
		free(src);
		strv_free(&f);
	}
	free(copy);
	/* drop the overmounted ones */
	size_t j = 0;
	for (size_t i = 0; i < out->n; i++) {
		if (!*out->v[i].dir) {
			free(out->v[i].spec);
			free(out->v[i].dir);
			free(out->v[i].type);
			free(out->v[i].opts);
			count--;
			continue;
		}
		out->v[j++] = out->v[i];
	}
	out->n = j;
	return count;
}

char *part_name(const char *disk, unsigned n)
{
	size_t l = strlen(disk);
	bool digit = l && isdigit((unsigned char)disk[l - 1]);
	return xasprintf("%s%s%u", disk, digit ? "p" : "", n);
}

const char *root_part_type(const char *arch)
{
	/* the Discoverable Partitions Specification (uapi-group.org) */
	static const char *const types[][2] = {
		{"x86_64", "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709"},
		{"aarch64", "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"},
		{"riscv64", "72EC70A6-CF74-40E6-BD49-4BDA08E8F224"},
		{"i686", "44479540-F297-41B2-9AF7-D131D5F0458A"},
		{"armv7l", "69DAD710-2CE4-4E3C-B16C-21A1D49ABED3"},
	};
	for (size_t i = 0; i < ARRAY_LEN(types); i++)
		if (!strcmp(arch, types[i][0]))
			return types[i][1];
	return "0FC63DAF-8483-4772-8E79-3D69D8477DE4"; /* Linux filesystem */
}

#define ESP_TYPE "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

char *sfdisk_script(unsigned esp_mib, const char *esp_uuid, const char *root_uuid, const char *root_type)
{
	return xasprintf("label: gpt\n"
			 "size=%uMiB, type=" ESP_TYPE ", uuid=%s, name=\"EFI system partition\"\n"
			 "type=%s, uuid=%s, name=\"CleanLinux\"\n",
			 esp_mib, esp_uuid, root_type, root_uuid);
}

void uuid_random(char out[37])
{
	unsigned char b[16];
	random_bytes(b, sizeof(b));
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40); /* version 4 */
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); /* RFC 4122 variant */
	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
		 b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

bool uuid_valid(const char *s)
{
	if (strlen(s) != 36)
		return false;
	for (int i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return false;
		} else if (!isxdigit((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int guid_bytes(const char *s, uint8_t out[16])
{
	if (!uuid_valid(s))
		return -1;
	uint8_t b[16];
	for (int i = 0, j = 0; i < 16; i++, j += 2) {
		if (s[j] == '-')
			j++;
		b[i] = (uint8_t)(hexval(s[j]) << 4 | hexval(s[j + 1]));
	}
	/* the first three fields are little endian in EFI */
	static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	for (int i = 0; i < 16; i++)
		out[i] = b[order[i]];
	return 0;
}

static void le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void le64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

size_t efi_load_option(uint8_t *buf, size_t cap, const char *desc, uint32_t partno, uint64_t start,
		       uint64_t size, const uint8_t guid[16], const char *path)
{
	size_t dl = strlen(desc), pl = strlen(path);
	bool slash = path[0] != '/' && path[0] != '\\';
	size_t file_len = 4 + 2 * (pl + slash + 1);
	size_t fpl = 42 + file_len + 4;
	size_t total = 4 + 2 + 2 * (dl + 1) + fpl;
	if (total > cap || fpl > 0xffff)
		return 0;
	memset(buf, 0, total);
	uint8_t *p = buf;
	le32(p, 1); /* LOAD_OPTION_ACTIVE */
	le16(p + 4, (uint16_t)fpl);
	p += 6;
	for (size_t i = 0; i <= dl; i++, p += 2)
		le16(p, (uint8_t)desc[i]); /* ASCII descriptions */
	/* hard drive media device path: the GPT partition */
	p[0] = 4;
	p[1] = 1;
	le16(p + 2, 42);
	le32(p + 4, partno);
	le64(p + 8, start);
	le64(p + 16, size);
	memcpy(p + 24, guid, 16);
	p[40] = 2; /* GPT */
	p[41] = 2; /* GUID signature */
	p += 42;
	/* file path media device path, backslashes */
	p[0] = 4;
	p[1] = 4;
	le16(p + 2, (uint16_t)file_len);
	p += 4;
	if (slash) {
		le16(p, '\\');
		p += 2;
	}
	for (size_t i = 0; i <= pl; i++, p += 2)
		le16(p, path[i] == '/' ? '\\' : (uint8_t)path[i]);
	/* end of the device path */
	p[0] = 0x7f;
	p[1] = 0xff;
	le16(p + 2, 4);
	return total;
}

char *shadow_set(const char *text, const char *user, const char *hash, long days)
{
	sbuf b = SBUF_INIT;
	size_t ul = strlen(user);
	bool done = false;
	for (const char *p = text; p && *p;) {
		size_t len = strcspn(p, "\n");
		if (!done && len > ul && !strncmp(p, user, ul) && p[ul] == ':') {
			/* name:password:lastchange:min:max:warn:inactive:expire:reserved */
			const char *rest = p + ul + 1, *end = p + len;
			const char *f2 = memchr(rest, ':', (size_t)(end - rest));
			const char *f3 = f2 ? memchr(f2 + 1, ':', (size_t)(end - f2 - 1)) : NULL;
			sb_printf(&b, "%s:%s:%ld", user, hash, days);
			if (f3)
				sb_addn(&b, f3, (size_t)(end - f3));
			else
				sb_add(&b, ":0:99999:7:::");
			sb_add(&b, "\n");
			done = true;
		} else {
			sb_addn(&b, p, len);
			sb_add(&b, "\n");
		}
		p += len;
		if (*p == '\n')
			p++;
	}
	if (!done)
		sb_printf(&b, "%s:%s:%ld:0:99999:7:::\n", user, hash, days);
	return sb_take(&b);
}

static bool needs_quotes(const char *v)
{
	if (!*v)
		return true;
	for (const char *c = v; *c; c++)
		if (!isalnum((unsigned char)*c) && !strchr("-_./:,+@%", *c))
			return true;
	return false;
}

char *conf_set(const char *text, const char *key, const char *value)
{
	sbuf b = SBUF_INIT;
	size_t kl = strlen(key);
	bool done = false;
	char *line_val = needs_quotes(value) ? xasprintf("%s=\"%s\"", key, value) : xasprintf("%s=%s", key, value);
	for (const char *p = text; p && *p;) {
		size_t len = strcspn(p, "\n");
		const char *s = p;
		while (s < p + len && (*s == ' ' || *s == '\t'))
			s++;
		if ((size_t)(p + len - s) > kl && !strncmp(s, key, kl) && s[kl] == '=') {
			if (!done)
				sb_printf(&b, "%s\n", line_val);
			done = true;
		} else {
			sb_addn(&b, p, len);
			sb_add(&b, "\n");
		}
		p += len;
		if (*p == '\n')
			p++;
	}
	if (!done)
		sb_printf(&b, "%s\n", line_val);
	free(line_val);
	return sb_take(&b);
}

char *cmdline_text(const char *root_partuuid, const char *rootflags, const char *extra)
{
	sbuf b = SBUF_INIT;
	sb_printf(&b, "root=PARTUUID=%s", root_partuuid);
	if (rootflags && *rootflags)
		sb_printf(&b, " rootflags=%s", rootflags);
	sb_add(&b, " rootwait ro");
	if (extra && *extra)
		sb_printf(&b, " %s", extra);
	return sb_take(&b);
}

bool valid_hostname(const char *s)
{
	size_t n = strlen(s);
	if (!n || n > 64 || *s == '-' || *s == '.' || s[n - 1] == '-' || s[n - 1] == '.')
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '.')
			return false;
	return true;
}

bool valid_username(const char *s)
{
	size_t n = strlen(s);
	if (!n || n > 32 || !strcmp(s, "root") || !(islower((unsigned char)*s) || *s == '_'))
		return false;
	for (; *s; s++)
		if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-')
			return false;
	return true;
}

char *size_text(uint64_t bytes)
{
	if (bytes >= (1ULL << 40))
		return xasprintf("%.1f TiB", (double)bytes / (double)(1ULL << 40));
	if (bytes >= (1ULL << 30))
		return xasprintf("%.1f GiB", (double)bytes / (double)(1ULL << 30));
	return xasprintf("%llu MiB", (unsigned long long)(bytes >> 20));
}
