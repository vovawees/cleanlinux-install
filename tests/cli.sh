#!/bin/sh
# Command line tests: sh tests/cli.sh ./cleanlinux-install. No root needed: what changes a disk
# runs only as --dry-run; the real installation is tested in a VM by CleanLinux (tests/install.py).
set -eu
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
# the tools of a CleanLinux system, as stand-ins
mkdir -p "$tmp/bin"
for t in burn cleanlinux-boot sfdisk mkfs.vfat mkfs.btrfs useradd udevadm; do
	printf '#!/bin/sh\necho "%s $*" >>"%s/ran"\n' "$t" "$tmp" >"$tmp/bin/$t"
	chmod 755 "$tmp/bin/$t"
done
ci() { env PATH="$tmp/bin:$PATH" "$BIN" "$@"; }

"$BIN" --version | grep -q '^cleanlinux-install ' || fail "--version"
"$BIN" --help | grep -q 'usage: cleanlinux-install' || fail "--help"
if "$BIN" 2>/dev/null; then fail "no disk and no target must fail"; fi
if "$BIN" -n a b 2>/dev/null; then fail "two disks must fail"; fi
if ci -n --hostname 'bad name' /dev/null 2>"$tmp/err"; then fail "bad hostname"; fi
grep -q 'hostname bad name' "$tmp/err" || fail "hostname message: $(cat "$tmp/err")"
if ci -n --user Root /dev/null 2>"$tmp/err"; then fail "bad user"; fi
grep -q 'user Root' "$tmp/err" || fail "user message"
if ci -n --user-password-file x /dev/null 2>"$tmp/err"; then fail "user password without a user"; fi
if ci -n --esp-size 10 /dev/null 2>"$tmp/err"; then fail "small ESP"; fi
grep -q 'esp-size' "$tmp/err" || fail "esp-size message"
if ci -n -t relative /dev/null 2>"$tmp/err"; then fail "relative target"; fi
if ci -n -t / 2>"$tmp/err"; then fail "target /"; fi
grep -q 'the running system itself' "$tmp/err" || fail "target / message"
if ci -n "$tmp/bin/burn" 2>"$tmp/err"; then fail "a file is not a disk"; fi
grep -q 'is not a block device' "$tmp/err" || fail "block device message: $(cat "$tmp/err")"
if env PATH=/nonexistent "$BIN" -n /dev/null 2>"$tmp/err"; then fail "missing tools"; fi
grep -q 'burn is missing' "$tmp/err" || fail "missing tool message: $(cat "$tmp/err")"
if [ "$(id -u)" != 0 ]; then
	if ci /dev/null 2>"$tmp/err"; then fail "not root"; fi
	grep -q 'run it as root' "$tmp/err" || fail "root message"
fi
mkdir -p "$tmp/notmounted"
if ci -n -t "$tmp/notmounted" 2>"$tmp/err"; then fail "target not mounted"; fi
grep -q 'is not a mount point' "$tmp/err" || fail "mount point message: $(cat "$tmp/err")"

# a dry run on a real disk of this machine (read only): the whole plan, nothing runs
disk=
for s in /sys/block/*; do
	n=${s##*/}
	case $n in loop* | ram* | zram* | sr* | dm-* | md* | nbd*) continue ;; esac
	[ -e "/dev/$n" ] && [ "$(cat "$s/size")" -gt 16777216 ] && [ "$(cat "$s/ro")" = 0 ] || continue
	disk=/dev/$n && break
done
if [ -n "$disk" ]; then
	ci -n -y -t "$tmp/target" --user anna --cmdline console=ttyS0 -p "base linux" "$disk" >"$tmp/out" 2>&1 ||
		fail "dry run on $disk: $(cat "$tmp/out")"
	for want in 'label: gpt' 'name="EFI system partition"' "sfdisk --quiet --wipe always --wipe-partitions always $disk" \
		'mkfs.btrfs -f -q -L CleanLinux' 'btrfs subvolumes: @ (/), @home (/home), @snapshots (/.snapshots)' \
		"burn -r $tmp/target -y clone / base linux" 'rootflags=subvol=@ rootwait ro console=ttyS0' \
		'subvol=@home,compress=zstd:1,noatime 0 0' 'umask=0077,noatime 0 2' 'tmpfs /tmp tmpfs nosuid,nodev 0 0' \
		"useradd -R $tmp/target -m -U -G wheel -s /bin/bash anna" "cleanlinux-boot --root $tmp/target kernel sync" \
		'Dry run: nothing was changed'; do
		grep -qF -- "$want" "$tmp/out" || fail "dry run: no '$want' in: $(cat "$tmp/out")"
	done
	[ ! -e "$tmp/ran" ] || fail "dry run ran: $(cat "$tmp/ran")"
	[ ! -e "$tmp/target" ] || fail "dry run created the target"
else
	echo "cli: no disk for the dry run, skipped"
fi
echo "cli: OK"
