#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The screens of cleanlinux-setup in a pseudo terminal: python3 tests/setup.py ./cleanlinux-setup

No root and no disk is touched: --dry-run, stand-in disks (CLEANLINUX_SETUP_DISKS), a small
kbd/tzdata tree (CLEANLINUX_SETUP_DATA) and a stand-in installer (CLEANLINUX_SETUP_INSTALLER)
that records its arguments and the password files and prints what cleanlinux-install prints.
A tiny terminal emulator keeps the screen, the tests look at it like a person would."""
import codecs
import fcntl
import gzip
import os
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time

ENTER, TAB, BTAB, ESC = "\r", "\t", "\x1b[Z", "\x1b"
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
CTRL_U, CTRL_C, BS = "\x15", "\x03", "\x7f"


class Screen:
    """What a terminal shows: the cursor movements, erasing and text of the TUI (SGR ignored)."""

    def __init__(self, rows, cols):
        self.resize(rows, cols)
        self.dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self.pending = ""

    def resize(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.y = self.x = 0

    def feed(self, data):
        s = self.pending + self.dec.decode(data)
        self.pending = ""
        i = 0
        while i < len(s):
            c = s[i]
            if c == "\x1b":
                m = re.match(r"\x1b\[([0-9;?]*)([@-~])", s[i:])
                if not m:
                    if len(s) - i < 16 and re.fullmatch(r"\x1b(\[[0-9;?]*)?", s[i:]):
                        self.pending = s[i:]  # the rest comes with the next read
                        return
                    i += 2
                    continue
                self.csi(m.group(1), m.group(2))
                i += m.end()
                continue
            if c == "\r":
                self.x = 0
            elif c == "\n":
                self.y = min(self.y + 1, self.rows - 1)
            elif c == "\b":
                self.x = max(self.x - 1, 0)
            elif c >= " ":
                if self.x >= self.cols:
                    self.x = 0
                    self.y = min(self.y + 1, self.rows - 1)
                self.grid[self.y][self.x] = c
                self.x += 1
            i += 1

    def csi(self, params, final):
        if params.startswith("?"):
            return
        p = [int(v) if v else 0 for v in params.split(";")] if params else []
        if final == "H":
            self.y = min(max((p[0] if p else 1) - 1, 0), self.rows - 1)
            self.x = min(max((p[1] if len(p) > 1 else 1) - 1, 0), self.cols - 1)
        elif final == "J" and (p[:1] == [2] or p[:1] == [3]):
            self.grid = [[" "] * self.cols for _ in range(self.rows)]
        elif final == "K":
            for x in range(self.x, self.cols):
                self.grid[self.y][x] = " "

    def text(self):
        return "\n".join("".join(r).rstrip() for r in self.grid)


class Tui:
    def __init__(self, argv, env, rows=30, cols=100):
        self.screen = Screen(rows, cols)
        master, slave = os.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        pid = os.fork()
        if pid == 0:
            try:
                os.close(master)
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
                for f in (0, 1, 2):
                    os.dup2(slave, f)
                if slave > 2:
                    os.close(slave)
                os.execve(argv[0], argv, env)
            finally:
                os._exit(127)
        os.close(slave)
        self.pid, self.fd, self.status, self.raw = pid, master, None, b""

    def pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            data = b""
        if not data:
            self.reap(block=True)
            return False
        self.raw += data
        self.screen.feed(data)
        return True

    def reap(self, block=False):
        if self.status is None:
            pid, st = os.waitpid(self.pid, 0 if block else os.WNOHANG)
            if pid == self.pid:
                self.status = os.waitstatus_to_exitcode(st)
        return self.status

    def wait_for(self, *texts, timeout=10, gone=False):
        """until every TEXT is on the screen (or none, with gone=True)"""
        end = time.monotonic() + timeout
        while True:
            scr = self.screen.text()
            if all((t not in scr) if gone else (t in scr) for t in texts):
                return scr
            if time.monotonic() > end or (self.status is not None and not self.pump(0)):
                raise AssertionError("waited for %r%s; the screen:\n%s" % (texts, " to go" if gone else "", scr))
            self.pump(0.05)

    def settle(self, t=0.15):
        end = time.monotonic() + t
        while time.monotonic() < end:
            self.pump(0.02)

    def send(self, *keys):
        for k in keys:
            os.write(self.fd, k.encode())
            # Esc alone: the TUI waits a little for the rest of a key sequence
            self.settle(0.12 if k == ESC else 0.03)

    def type(self, text):
        for c in text:
            self.send(c)

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.screen.resize(rows, cols)
        os.kill(self.pid, signal.SIGWINCH)

    def finish(self, timeout=10):
        end = time.monotonic() + timeout
        while self.reap() is None and time.monotonic() < end:
            self.pump(0.05)
        if self.status is None:
            os.kill(self.pid, signal.SIGKILL)
            self.reap(block=True)
            raise AssertionError("cleanlinux-setup did not exit; the screen:\n" + self.screen.text())
        while self.pump(0):
            pass
        os.close(self.fd)
        return self.status


def check(cond, what, scr=""):
    if not cond:
        raise AssertionError(what + ("\n" + scr if scr else ""))


def make_data(root):
    """kbd keymaps and fonts and a zone.tab, as a CleanLinux live system has them"""
    for m in ("i386/qwerty/us", "i386/qwerty/ruwin_alt_sh-UTF-8", "i386/qwertz/de-latin1", "i386/dvorak/dvorak"):
        p = os.path.join(root, "usr/share/kbd/keymaps", m + ".map.gz")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with gzip.open(p, "wb") as f:
            f.write(b"keymaps 0-2\n")
    for font in ("latarcyrheb-sun16.psfu.gz", "lat9w-16.psfu.gz"):
        p = os.path.join(root, "usr/share/kbd/consolefonts", font)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").close()
    p = os.path.join(root, "usr/share/zoneinfo/zone.tab")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write("# tzdata zone.tab\n")
        for cc, coord, zone in (("RU", "+5545+03735", "Europe/Moscow"), ("DE", "+5230+01322", "Europe/Berlin"),
                                ("RU", "+5650+06036", "Asia/Yekaterinburg"), ("US", "+404251-0740023", "America/New_York"),
                                ("AR", "-3436-05827", "America/Argentina/Buenos_Aires"), ("JP", "+353916+1394441", "Asia/Tokyo")):
            f.write("%s\t%s\t%s\n" % (cc, coord, zone))


def make_medium(root):
    """the live medium as cleanlinux-boot mounts it: a spin (labwc) and the proprietary drivers"""
    spin = os.path.join(root, "spins/labwc/var/lib/burn/spins/labwc")
    os.makedirs(spin)
    with open(os.path.join(spin, "spin.conf"), "w") as f:
        f.write('SPIN_NAME=labwc\nSPIN_DESC="labwc: a light Wayland desktop with a panel"\n'
                'SPIN_DESC_RU="labwc: лёгкий рабочий стол Wayland с панелью"\n')
    os.makedirs(os.path.join(root, "nonfree/var/lib/burn/installed/steamdeck-dsp"))
    os.makedirs(os.path.join(root, "@/usr"))


INSTALLER = r"""#!/bin/sh
# a stand-in for cleanlinux-install: the arguments, the passwords, the progress of a real one
out=%(out)s
printf '%%s\n' "$@" >"$out/args"
prev=
for a; do
	case $prev in
	--root-password-file | --user-password-file)
		printf '%%s %%s %%s\n' "$prev" "$(stat -c %%a "$a")" "$(cat "$a")" >>"$out/secrets"
		dirname "$a" >"$out/secretdir"
		;;
	esac
	prev=$a
done
step() { printf '\033[1;36m::\033[0m %%s\n' "$1"; sleep 0.05; }
step "Disk /dev/vdb"
step "Partitioning /dev/vdb"
step "Formatting"
step "Mounting"
step "Copying the system (2.4 GiB in 51234 files, nothing is built or downloaded)"
for p in 10 50 100; do echo "==> Copied $p%% ($p MiB of 2.4 GiB)"; sleep 0.3; done
if [ -n "${FAKE_FAIL:-}" ]; then
	echo "error: something broke"
	exit "$FAKE_FAIL"
fi
step "Configuring"
step "Users"
step "Kernel and boot loader"
step "Unmounting"
step "Done: CleanLinux is installed on /dev/vdb"
"""


def base_env(tmp, data, extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLEANLINUX_")}
    env.update({"TERM": "xterm-256color", "LANG": "C.UTF-8", "CLEANLINUX_SETUP_DATA": data,
                "CLEANLINUX_SETUP_INSTALLER": os.path.join(tmp, "installer"),
                "CLEANLINUX_SETUP_MEDIUM": os.path.join(tmp, "medium"),
                "CLEANLINUX_SETUP_DISKS": "vdb=20480=QEMU HARDDISK=ext4, vfat;sdz=2048=Tiny stick;"
                                          "sdy=8192=Busy disk==busy;sdx=16384=Live stick==medium",
                "PATH": "/usr/bin:/bin:/usr/sbin:/sbin"})
    env.update(extra or {})
    return env


def test_english(binary, tmp, data):
    """the whole way in English: the labwc desktop, no proprietary drivers, a dry run"""
    out = os.path.join(tmp, "en")
    os.makedirs(out)
    with open(os.path.join(tmp, "installer"), "w") as f:
        f.write(INSTALLER % {"out": out})
    os.chmod(os.path.join(tmp, "installer"), 0o755)
    t = Tui([binary, "--dry-run"], base_env(tmp, data))
    t.wait_for("Language · Язык", "English", "Русский", "CleanLinux installation (dry run)", "step 1 of 8",
               "Dry run: cleanlinux-install only shows")
    # the steps on the left of a large screen
    scr = t.wait_for("▶ Language", "2 Keyboard", "6 System", "8 Installation", "Enter")
    t.send(ENTER)
    t.wait_for("Keyboard layout", "English (US)", "German", "Russian", "step 2 of 8", "√ Language", "▶ Keyboard")
    check("Ukrainian" not in t.screen.text(), "a keymap kbd does not have is not offered", t.screen.text())
    # searching: by the name and by the keymap
    t.type("germ")
    scr = t.wait_for("Search: germ", "German")
    check("English (US)" not in scr, "the search hides the others", scr)
    t.send(CTRL_U)
    t.type("dvo")
    t.wait_for("English (Dvorak)")
    t.send(CTRL_U, ESC)  # Esc with an empty search: back to the language
    t.wait_for("Language · Язык", "step 1 of 8")
    t.send(ENTER)
    t.wait_for("Keyboard layout")
    t.type("de-lat")
    t.wait_for("German", "de-latin1")
    t.send(ENTER)
    # time zone: the region, then the city
    scr = t.wait_for("Time zone", "Choose the region.", "UTC (no time zone)", "Europe", "America")
    check("Africa" not in scr, "regions without zones are not offered", scr)
    t.type("eur")
    t.wait_for("Search: eur")
    t.send(ENTER)
    t.wait_for("Time zone: Europe", "Berlin", "Moscow")
    t.send(ESC)  # back to the regions
    t.wait_for("Choose the region.")
    t.send(ENTER)
    t.wait_for("Time zone: Europe")
    t.type("mos")
    t.wait_for("Search: mos")
    t.send(ENTER)
    # the disk: the small, busy and live ones cannot be chosen
    scr = t.wait_for("Disk", "/dev/vdb  QEMU HARDDISK", "20.0 GiB", "/dev/sdz  Tiny stick", "Look again",
                     "on it now: ext4, vfat")
    t.send(DOWN)
    scr = t.wait_for("too small: at least 3.5 GiB")
    t.send(DOWN)
    t.wait_for("in use (mounted)")
    t.send(DOWN)
    t.wait_for("the installation medium")
    t.send(ENTER)  # a disabled one: nothing happens
    t.settle(0.3)
    check("the installation medium" in t.screen.text(), "a disabled disk cannot be chosen", t.screen.text())
    t.send(DOWN)
    t.wait_for("▶ Look again")
    t.send(ENTER)  # look again: the list comes back
    t.wait_for("/dev/vdb  QEMU HARDDISK")
    t.send("\x1b[H")  # Home
    t.send(ENTER)
    # the user: wrong answers first
    t.wait_for("Computer and user", "Computer name", "cleanlinux", "root, gets the same password", "step 5 of 8")
    t.send(CTRL_U)
    t.type("testbox")
    t.send(TAB)
    t.type("Anna")
    t.send(TAB)
    t.type("pw1")
    t.send(TAB)
    t.type("pw2")
    t.send(ENTER)
    t.wait_for("User name: small Latin letters")
    t.send(CTRL_U)
    t.type("root")
    t.send(ENTER, ENTER, ENTER)
    t.wait_for("User name: small Latin letters")
    t.send(CTRL_U)
    t.type("anna")
    t.send(ENTER, ENTER, ENTER)
    t.wait_for("The two passwords differ.")
    t.send(CTRL_U)
    t.type("pw1")
    scr = t.wait_for("Computer and user")
    check("pw1" not in scr and "pw2" not in scr, "passwords are not shown", scr)
    t.send(ENTER)
    # the system: the desktop of the medium, the proprietary drivers
    scr = t.wait_for("System", "Desktop", "◄ labwc", "Proprietary drivers", "◄ on", "step 6 of 8",
                     "labwc: a light Wayland desktop with a panel")
    t.send(RIGHT)
    t.wait_for("none (console)", "Only the text console.")
    t.send(LEFT)
    t.wait_for("◄ labwc", "labwc: a light Wayland desktop")
    t.send(DOWN)
    t.wait_for("not free software (steamdeck-dsp)", "ALLOW_NONFREE=1")
    t.send(" ")
    t.wait_for("◄ off", "Only free software", "ALLOW_NONFREE=0")
    t.send(ENTER)
    scr = t.wait_for("Ready to install", "/dev/vdb  QEMU HARDDISK  20.0 GiB", "German (de-latin1)", "Europe/Moscow",
                     "testbox", "anna", "labwc", "off", "ALL DATA ON /dev/vdb WILL BE ERASED.", "< Install >",
                     "step 7 of 8")
    t.send(ENTER)
    t.wait_for("Erase the disk?", "Everything on /dev/vdb (QEMU HARDDISK)", "Erase and install")
    t.send(ENTER)  # Back is the default
    t.wait_for("Ready to install")
    t.send(ENTER, RIGHT, ENTER)
    scr = t.wait_for("Installing", "Copying the system: ", ":: Mounting", "step 8 of 8", timeout=15)
    check("[1;36m" not in scr, "the log tail without colors", scr)
    t.wait_for("Copying the system: 50% · 50 MiB of 2.4 GiB", timeout=15)
    scr = t.wait_for("CleanLinux is installed", "Remove the USB stick", "The desktop (labwc) starts",
                     "Exit to the console", "√ Installation", timeout=20)
    args = open(os.path.join(out, "args")).read().split("\n")
    want = ["-y", "-n", "--hostname", "testbox", "--user", "anna", "--root-password-file"]
    check(args[:len(want)] == want, "installer arguments: %r" % args)
    for a, b in (("--timezone", "Europe/Moscow"), ("--keymap", "de-latin1"), ("--font", "lat9w-16"),
                 ("--spin", "labwc")):
        check(a in args and args[args.index(a) + 1] == b, "%s %s in %r" % (a, b, args))
    check("--no-nonfree" in args and "--encrypt" not in args and args[-2] == "/dev/vdb",
          "no proprietary drivers, the disk last: %r" % args)
    secrets = open(os.path.join(out, "secrets")).read()
    check("--root-password-file 600 pw1\n" in secrets and "--user-password-file 600 pw1\n" in secrets,
          "the password files: %r" % secrets)
    sdir = open(os.path.join(out, "secretdir")).read().strip()
    check(not os.path.exists(sdir), "the password files are removed: " + sdir)
    log = open("/tmp/cleanlinux-install.log").read()
    check("cleanlinux-setup: " in log and "==> Copied 100%" in log, "the log: " + log)
    check("pw1" not in log, "no password in the log")
    t.send(RIGHT, RIGHT, ENTER)
    status = t.finish()
    check(status == 0, "exit status %s" % status)
    check(b"to start the installer again: cleanlinux-setup" in t.raw, "the last words")
    check(t.raw.rstrip().endswith(b"cleanlinux-setup"), "the screen is given back")
    print("setup: English, a dry run: OK")


def test_russian(binary, tmp, data):
    """Russian, the console only, the installer fails, ASCII lines, a resize"""
    out = os.path.join(tmp, "ru")
    os.makedirs(out)
    with open(os.path.join(tmp, "installer"), "w") as f:
        f.write(INSTALLER % {"out": out})
    t = Tui([binary, "--dry-run", "--ascii"], base_env(tmp, data, {"FAKE_FAIL": "3"}), rows=24, cols=80)
    t.wait_for("Language . Язык", "> English")
    t.send(DOWN, ENTER)
    scr = t.wait_for("Раскладка клавиатуры", "> Русская", "Alt+Shift", "Установка CleanLinux (пробный запуск)",
                     "шаг 2 из 8")
    check(not re.search("[\u2500-\u257f]", scr), "--ascii: no box drawing", scr)
    check("Клавиатура" not in scr, "no list of steps at 80x24", scr)
    t.send(ENTER)
    # the regions in Russian are found by the Latin names too
    t.wait_for("Часовой пояс", "Европа", "Europe")
    t.type("eur")
    t.wait_for("Поиск: eur")
    t.send(ENTER)
    t.wait_for("Часовой пояс: Европа")
    t.resize(30, 100)
    scr = t.wait_for("Часовой пояс: Европа", "Moscow", "+ Язык", "+ Клавиатура", "> Часовой пояс", "4 Диск")
    check(len(scr.split("\n")) == 30 and any(len(l) > 80 for l in scr.split("\n")), "redrawn at 100x30", scr)
    t.type("berl")
    t.send(ENTER)
    t.wait_for("Диск", "/dev/vdb")
    t.send(ENTER)
    t.wait_for("Компьютер и пользователь", "Администратор root получит тот же пароль")
    t.send(TAB)
    t.type("bob")
    t.send(TAB)
    t.type("secret")
    t.send(TAB)
    t.type("secret")
    t.send(ENTER)
    t.wait_for("Система", "Рабочий стол", "< labwc", "лёгкий рабочий стол Wayland", "Проприетарные драйверы",
               "< включены")
    t.send(RIGHT)
    t.wait_for("нет (консоль)", "Только текстовая консоль")
    t.send(ENTER)
    scr = t.wait_for("Всё готово к установке", "Europe/Berlin", "нет (консоль)", "включены",
                     "ВСЕ ДАННЫЕ НА /dev/vdb БУДУТ СТЁРТЫ.")
    t.send(ENTER, RIGHT, ENTER)
    scr = t.wait_for("Установка не удалась", "код 3", "error: something broke", timeout=20)
    args = open(os.path.join(out, "args")).read().split("\n")
    check("--spin" not in args and "--no-nonfree" not in args and "--encrypt" not in args,
          "the console, the drivers: %r" % args)
    check(args[args.index("--keymap") + 1] == "ruwin_alt_sh-UTF-8" and args[args.index("--font") + 1] == "latarcyrheb-sun16",
          "the Russian keymap and a Cyrillic font: %r" % args)
    secrets = open(os.path.join(out, "secrets")).read()
    for want in ("--root-password-file 600 secret", "--user-password-file 600 secret"):
        check(want in secrets, "%r in %r" % (want, secrets))
    t.send(ENTER)  # back to the summary
    t.wait_for("Всё готово к установке")
    t.send(CTRL_C)
    t.wait_for("Выйти?", "Ничего не изменено")
    t.send(RIGHT, ENTER)
    status = t.finish()
    check(status == 0, "exit status %s" % status)
    check("снова запустить установщик".encode() in t.raw, "the last words in Russian")
    print("setup: Russian, the console only, a failure: OK")


def test_misc(binary, tmp, data):
    """no disk, no live medium; --help; --auto off the main console; a terminal that is too small"""
    env = base_env(tmp, data, {"CLEANLINUX_SETUP_DISKS": "sdz=1024=Tiny",
                               "CLEANLINUX_SETUP_MEDIUM": os.path.join(tmp, "nothing")})
    t = Tui([binary, "--dry-run", "--lang", "en"], env)
    t.wait_for("Language · Язык")
    t.send(ENTER, ENTER)
    t.wait_for("Time zone")
    t.send(ENTER)  # UTC: the default here
    scr = t.wait_for("No disk to install on", "▶ Look again")
    t.send(ESC)
    t.wait_for("Time zone")
    t.send(CTRL_C)
    t.wait_for("Quit?")
    t.send(ENTER)  # Continue
    t.wait_for("Time zone")
    t.send(CTRL_C, RIGHT, ENTER)
    check(t.finish() == 0, "quit")

    t = Tui([binary, "--help"], base_env(tmp, data))
    check(t.finish() == 0 and b"usage: cleanlinux-setup" in t.raw, "--help")
    t = Tui([binary, "--bogus"], base_env(tmp, data))
    check(t.finish() == 2, "a bad option")
    if os.geteuid() != 0:
        t = Tui([binary], base_env(tmp, data))
        check(t.finish() == 1 and b"run it as root" in t.raw, "not root")
    t = Tui([binary, "--auto", "--dry-run"], base_env(tmp, data))
    st = t.finish()
    check(st == 0, "--auto: %s %r" % (st, t.raw))
    t = Tui([binary, "--dry-run"], base_env(tmp, data), rows=10, cols=40)
    t.wait_for("Language")
    t.send(CTRL_C, RIGHT, ENTER)
    check(t.finish() == 0, "a small terminal")
    print("setup: no disk, options, --auto: OK")


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./cleanlinux-setup")
    with tempfile.TemporaryDirectory() as tmp:
        data = os.path.join(tmp, "data")
        make_data(data)
        make_medium(os.path.join(tmp, "medium"))
        for test in (test_english, test_russian, test_misc):
            test(binary, tmp, data)
    print("setup: OK")


if __name__ == "__main__":
    main()
