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
# this computer, as burn profile detect sees it (a query: a dry run may ask it)
# shellcheck disable=SC2016 # the stub's own $1 $2 $*, expanded when it runs
printf '#!/bin/sh\nif [ "$1 $2" = "profile detect" ]; then printf "arch       x86_64\\nprofile    steam-deck\\n"; exit 0; fi\necho "burn $*" >>"%s/ran"\n' \
	"$tmp" >"$tmp/bin/burn"
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
grep -q 'cleanlinux-boot is missing' "$tmp/err" || fail "missing tool message: $(cat "$tmp/err")"
if [ "$(id -u)" != 0 ]; then
	if ci /dev/null 2>"$tmp/err"; then fail "not root"; fi
	grep -q 'run it as root' "$tmp/err" || fail "root message"
fi
mkdir -p "$tmp/notmounted"
if ci -n -t "$tmp/notmounted" 2>"$tmp/err"; then fail "target not mounted"; fi
grep -q 'is not a mount point' "$tmp/err" || fail "mount point message: $(cat "$tmp/err")"
if ci -n --encrypt /dev/null 2>/dev/null; then fail "--encrypt is gone (no encryption in the base system)"; fi
if ci -n --root-password-file - --user anna --user-password-file - /dev/null 2>"$tmp/err"; then
	fail "two passwords from stdin"
fi
grep -q 'only one password can come from stdin' "$tmp/err" || fail "stdin message: $(cat "$tmp/err")"
if ci -n --timezone ../../etc/passwd /dev/null 2>"$tmp/err"; then fail "bad time zone"; fi
grep -q 'timezone ../../etc/passwd: a zone of tzdata' "$tmp/err" || fail "time zone message: $(cat "$tmp/err")"
if ci -n --keymap 'us/../x' /dev/null 2>"$tmp/err"; then fail "bad keymap"; fi
grep -q 'keymap us/../x' "$tmp/err" || fail "keymap message: $(cat "$tmp/err")"
if ci -n --font '-x' /dev/null 2>"$tmp/err"; then fail "bad font"; fi
grep -q 'font -x' "$tmp/err" || fail "font message: $(cat "$tmp/err")"
if ci -n --spin 'a/b' /dev/null 2>"$tmp/err"; then fail "bad spin"; fi
grep -q 'spin a/b: the name of a spin' "$tmp/err" || fail "spin message: $(cat "$tmp/err")"
# a live medium: the system tree @ with the layers next to it, as cleanlinux-boot mounts it
m=$tmp/medium
mkdir -p "$m/@/usr/bin" "$m/@/etc/burn" "$m/nonfree/var/lib/burn/installed/steamdeck-dsp" "$m/spins/labwc/usr/bin"
printf 'PROFILE=generic-x86_64\nFIRMWARE="amdgpu/ i915/"\n' >"$m/@/etc/burn/burn.conf"
echo labwc >"$m/spins/labwc/usr/bin/labwc"
if [ ! -d /run/cleanlinux/live/medium/@/usr ]; then
	if ci -n --spin labwc /dev/null 2>"$tmp/err"; then fail "a spin without a live medium"; fi
	grep -q 'spins come from the live medium' "$tmp/err" || fail "spin without a medium: $(cat "$tmp/err")"
fi
if ci -n --source "$tmp/notmounted" /dev/null 2>"$tmp/err"; then fail "a source without a system"; fi
grep -q 'has no system' "$tmp/err" || fail "source message: $(cat "$tmp/err")"
if ci -n --clone --source "$m/@" /dev/null 2>"$tmp/err"; then fail "--clone and --source"; fi
grep -q 'one of them' "$tmp/err" || fail "clone and source message: $(cat "$tmp/err")"
if ci -n --clone --spin labwc /dev/null 2>"$tmp/err"; then fail "--clone and --spin"; fi
grep -q 'spin needs the copy of a live medium' "$tmp/err" || fail "clone and spin message: $(cat "$tmp/err")"

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
		'burn profile detect: steam-deck (the running system builds for ' \
		'--profile steam-deck makes it the profile of the new system' 'Dry run: nothing was changed' \
		'proprietary drivers allowed (ALLOW_NONFREE=1)'; do
		grep -qF -- "$want" "$tmp/out" || fail "dry run: no '$want' in: $(cat "$tmp/out")"
	done
	[ ! -e "$tmp/ran" ] || fail "dry run ran: $(cat "$tmp/ran")"
	[ ! -e "$tmp/target" ] || fail "dry run created the target"
	# the time zone and the console of the new system
	ci -n -y -t "$tmp/target" --timezone Europe/Moscow --keymap ruwin_alt_sh-UTF-8 --font cyr-sun16 "$disk" \
		>"$tmp/out" 2>&1 || fail "dry run with a time zone: $(cat "$tmp/out")"
	for want in 'time zone Europe/Moscow, keymap ruwin_alt_sh-UTF-8, font cyr-sun16' \
		'time zone Europe/Moscow: /etc/localtime -> ../usr/share/zoneinfo/Europe/Moscow' \
		"write $tmp/target/etc/vconsole.conf" 'KEYMAP=ruwin_alt_sh-UTF-8' 'FONT=cyr-sun16'; do
		grep -qF -- "$want" "$tmp/out" || fail "time zone dry run: no '$want' in: $(cat "$tmp/out")"
	done
	[ ! -e "$tmp/ran" ] || fail "time zone dry run ran: $(cat "$tmp/ran")"
	# the copy of a live medium: the system, the proprietary drivers, a spin
	ci -n -y -t "$tmp/target" --source "$m/@" --spin labwc "$disk" >"$tmp/out" 2>&1 ||
		fail "copy dry run on $disk: $(cat "$tmp/out")"
	for want in "the system: $m/@" "the proprietary drivers: $m/nonfree" "the spin labwc: $m/spins/labwc" \
		'Copying the system (' 'proprietary drivers allowed (ALLOW_NONFREE=1)' 'rootflags=subvol=@ rootwait ro' \
		"cleanlinux-boot --root $tmp/target kernel sync"; do
		grep -qF -- "$want" "$tmp/out" || fail "copy dry run: no '$want' in: $(cat "$tmp/out")"
	done
	if grep -q 'burn -r' "$tmp/out"; then fail "copy dry run installs packages: $(cat "$tmp/out")"; fi
	ci -n -y -t "$tmp/target" --source "$m/@" --no-nonfree "$disk" >"$tmp/out" 2>&1 ||
		fail "free copy dry run on $disk: $(cat "$tmp/out")"
	grep -qF 'no proprietary packages (ALLOW_NONFREE=0)' "$tmp/out" || fail "--no-nonfree: $(cat "$tmp/out")"
	if grep -qF 'the proprietary drivers:' "$tmp/out"; then fail "--no-nonfree copies them: $(cat "$tmp/out")"; fi
	if ci -n -y -t "$tmp/target" --source "$m/@" --spin xfce "$disk" >"$tmp/out" 2>&1; then fail "a spin the medium lacks"; fi
	grep -qF 'the medium has no such spin' "$tmp/out" || fail "missing spin: $(cat "$tmp/out")"
	[ ! -e "$tmp/ran" ] || fail "copy dry run ran: $(cat "$tmp/ran")"
	[ ! -e "$tmp/target" ] || fail "copy dry run created the target"
else
	echo "cli: no disk for the dry run, skipped"
fi
echo "cli: OK"
