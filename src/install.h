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
/* the profile line of `burn profile detect`; NULL without one */
char *detected_profile(const char *out);
/* the kernel command line of the new system: ROOT is the value of root= (PARTUUID=… or a device);
 * with LUKS_UUID the initramfs first opens that LUKS container (rd.luks.uuid=) */
char *cmdline_text(const char *root, const char *luks_uuid, const char *rootflags, const char *extra);
/* the LUKS UUID of a dm-crypt mapping from its device-mapper UUID ("CRYPT-LUKS2-<32 hex>-<name>",
 * /sys/class/block/dm-N/dm/uuid); NULL when the mapping is not a LUKS container */
char *luks_uuid_of_dm(const char *dm_uuid);
/* the mapping the initramfs of the new system opens it as: /dev/mapper/luks-<UUID> */
char *luks_mapper(const char *luks_uuid);
bool valid_hostname(const char *s);
bool valid_username(const char *s);
char *size_text(uint64_t bytes);

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
char *luks_uuid_of(const char *dev); /* the LUKS UUID when DEV is an open LUKS container (dm-crypt) */
int btrfs_subvol_create(const char *parent, const char *name);
int efi_boot_entry(const struct disk *d, unsigned partno, const char *partuuid, const char *loader, const char *desc);
char *hash_password(const char *pw);
/* asked twice on the terminal; OPTION names the file option that replaces the question */
char *ask_password(const char *who, const char *option);
char *read_password_file(const char *path);
bool ask_yes(const char *question);

#endif
