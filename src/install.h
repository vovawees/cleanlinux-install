/* SPDX-License-Identifier: MIT */
#ifndef CLI_INSTALL_H
#define CLI_INSTALL_H

#include <stdint.h>

#include "util.h"

#ifndef VERSION
#define VERSION "0"
#endif

/* the mount options of the btrfs subvolumes and the ESP (the same as the CleanLinux image) */
#define BTRFS_OPTS "compress=zstd:1,noatime"
#define ESP_OPTS "umask=0077,noatime"

/* plan.c: the pure parts, unit tested */
struct fsent {
	char *spec, *dir, *type, *opts;
	int pass;
};
typedef struct {
	struct fsent *v;
	size_t n, cap;
} fsents;
void fsents_add(fsents *f, const char *spec, const char *dir, const char *type, const char *opts, int pass);
void fsents_free(fsents *f);
char *fstab_text(const fsents *f);
/* the filesystems mounted at TARGET and below, from /proc/self/mountinfo text; SPEC gives the
 * fstab name of a device ("PARTUUID=…"), or NULL to use the device path */
typedef char *(*spec_fn)(const char *dev, void *ctx);
int mountinfo_fsents(const char *mountinfo, const char *target, spec_fn spec, void *ctx, fsents *out);
char *mountinfo_unescape(const char *s);

char *part_name(const char *disk, unsigned n); /* sda,1 -> sda1; nvme0n1,1 -> nvme0n1p1 */
const char *root_part_type(const char *arch);  /* the root partition type of the architecture */
char *sfdisk_script(unsigned esp_mib, const char *esp_uuid, const char *root_uuid, const char *root_type);
void uuid_random(char out[37]);
bool uuid_valid(const char *s);
int guid_bytes(const char *s, uint8_t out[16]); /* EFI byte order (mixed endian) */
/* an EFI_LOAD_OPTION: PATH on the GPT partition PARTNO (LBAs START, SIZE; unique GUID) */
size_t efi_load_option(uint8_t *buf, size_t cap, const char *desc, uint32_t partno, uint64_t start,
		       uint64_t size, const uint8_t guid[16], const char *path);
/* /etc/shadow text with USER's password set to HASH (changed DAYS after the epoch) */
char *shadow_set(const char *text, const char *user, const char *hash, long days);
char *user_groups(const char *group_text);
/* KEY=value set in a shell-style configuration text (replaced or appended) */
char *conf_set(const char *text, const char *key, const char *value);
/* the value of KEY in such a text (the last assignment, without quotes); NULL when unset */
char *conf_get(const char *text, const char *key);
/* TEXT with its KEY= lines commented out */
char *conf_unset(const char *text, const char *key);
/* the profile line of `burn profile detect`; NULL without one */
char *detected_profile(const char *out);
/* the kernel command line of the new system: ROOT is the value of root= (PARTUUID=… or a device) */
char *cmdline_text(const char *root, const char *rootflags, const char *extra);
bool valid_hostname(const char *s);
bool valid_username(const char *s);
bool valid_zone(const char *s);         /* Europe/Moscow, America/Argentina/Buenos_Aires, UTC */
bool valid_console_name(const char *s); /* a keymap or console font of kbd: us, ruwin_alt_sh-UTF-8 */
char *size_text(uint64_t bytes);

/* copy.c: system trees copied as overlayfs shows its layers: a whiteout (a character device 0:0)
 * removes what is below, an opaque directory (trusted.overlay.opaque=y) replaces it */
struct tree_size {
	uint64_t bytes, entries; /* bytes of the regular files, every entry */
};
void tree_measure(const char *src, struct tree_size *t);
typedef void (*tree_progress)(uint64_t bytes, uint64_t entries, void *ctx);
/* the contents of SRC into DST, merged with what is there: owners, modes, times, extended
 * attributes, hard links and device nodes kept; other filesystems below SRC are not entered.
 * The number of entries that failed (each reported) */
int tree_copy(const char *src, const char *dst, tree_progress cb, void *ctx);

/* the progress of an installation, from the lines cleanlinux-install (and burn clone) print */
enum {
	STEP_NONE,
	STEP_DISK,
	STEP_PARTITION,
	STEP_FORMAT,
	STEP_MOUNT,
	STEP_PACKAGES, /* the copy of the system (or burn clone) */
	STEP_CONFIG,
	STEP_USERS,
	STEP_BOOT,
	STEP_UNMOUNT,
	STEP_DONE,
};
struct progress {
	int percent, step, pkgs, pkgs_done, copied; /* copied: percent of the system copied */
	char pkg[64]; /* the package being installed, or the amount copied */
};
void progress_line(struct progress *p, const char *line);
/* LINE without terminal escapes (colors) and trailing blanks, tabs as spaces, in OUT */
void plain_line(char *out, size_t cap, const char *line);

/* system.c: what touches the machine */
struct disk {
	char *dev;   /* /dev/vdb */
	char *name;  /* vdb */
	char *sys;   /* /sys/class/block/vdb */
	uint64_t bytes;
	unsigned lbs; /* logical block size */
	char *model;
	bool removable;
};
void disk_open(const char *path, struct disk *d, bool check_busy);
void disk_describe(const struct disk *d);
char *disk_part_dev(const struct disk *d, unsigned n, int wait_ms); /* /dev/vdb1 */
uint64_t sysfs_u64(const char *dir, const char *file);
char *partuuid_of(const char *dev, void *ctx); /* "PARTUUID=…" via libblkid, or NULL */
char *fstype_of(const char *dev);
int btrfs_subvol_create(const char *parent, const char *name);
int efi_boot_entry(const struct disk *d, unsigned partno, const char *partuuid, const char *loader, const char *desc);
char *hash_password(const char *pw);
/* asked twice on the terminal; OPTION names the file option that replaces the question */
char *ask_password(const char *who, const char *option);
char *read_password_file(const char *path);
bool ask_yes(const char *question);
/* the file of a keymap or console font of kbd under ROOT (KIND: keymaps, consolefonts), or NULL */
char *kbd_file(const char *root, const char *kind, const char *name);

#endif
