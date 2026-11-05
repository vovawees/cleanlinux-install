#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pictures of the screens of cleanlinux-setup for the README: sudo python3 tools/screens.py OUTDIR

The installer runs as in tests/setup.py (a dry run in a pseudo terminal, stand-in disks, a medium
with the labwc spin and the proprietary drivers), the keys of a person go in, and the screen is
drawn as the Linux console draws it: its 16 colours, bold as bright, 100x30 cells."""
import os
import re
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
import setup as t  # noqa: E402

VGA = [(0, 0, 0), (170, 0, 0), (0, 170, 0), (170, 85, 0), (0, 0, 170), (170, 0, 170), (0, 170, 170), (170, 170, 170),
       (85, 85, 85), (255, 85, 85), (85, 255, 85), (255, 255, 85), (85, 85, 255), (255, 85, 255), (85, 255, 255),
       (255, 255, 255)]


class ColorScreen(t.Screen):
    """the cells with their colours (SGR 0, 1, 7, 30-37, 40-47)"""

    def resize(self, rows, cols):
        super().resize(rows, cols)
        self.attr = (7, 0)
        self.colors = [[self.attr] * cols for _ in range(rows)]
        self.fg, self.bg, self.bold, self.rev = 7, 0, False, False

    def feed(self, data):
        # the text through the parent; the colours by replaying the SGR sequences here
        s = self.dec.decode(data)
        i = 0
        while i < len(s):
            c = s[i]
            if c == "\x1b":
                m = re.match(r"\x1b\[([0-9;?]*)([@-~])", s[i:])
                if not m:
                    i += 2
                    continue
                if m.group(2) == "m":
                    self.sgr(m.group(1))
                else:
                    self.csi(m.group(1), m.group(2))
                    if m.group(2) == "J":
                        self.colors = [[(7, 0)] * self.cols for _ in range(self.rows)]
                i += m.end()
                continue
            if c == "\r":
                self.x = 0
            elif c == "\n":
                self.y = min(self.y + 1, self.rows - 1)
            elif c >= " ":
                if self.x >= self.cols:
                    self.x, self.y = 0, min(self.y + 1, self.rows - 1)
                fg = self.fg + (8 if self.bold else 0)
                self.grid[self.y][self.x] = c
                self.colors[self.y][self.x] = (self.bg, fg) if self.rev else (fg, self.bg)
                self.x += 1
            i += 1

    def sgr(self, params):
        for p in [int(v) if v else 0 for v in params.split(";")] if params else [0]:
            if p == 0:
                self.fg, self.bg, self.bold, self.rev = 7, 0, False, False
            elif p == 1:
                self.bold = True
            elif p == 7:
                self.rev = True
            elif 30 <= p <= 37:
                self.fg = p - 30
            elif 40 <= p <= 47:
                self.bg = p - 40


def font(size):
    for f in ("/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
              "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def render(scr, path, cw=10, ch=20):
    img = Image.new("RGB", (scr.cols * cw, scr.rows * ch))
    d = ImageDraw.Draw(img)
    f = font(16)
    for y in range(scr.rows):
        for x in range(scr.cols):
            fg, bg = scr.colors[y][x]
            d.rectangle([x * cw, y * ch, x * cw + cw - 1, y * ch + ch - 1], fill=VGA[bg])
            c = scr.grid[y][x]
            if c in "─│┌┐└┘":  # box drawing as lines, cell to cell
                mx, my = x * cw + cw // 2, y * ch + ch // 2
                col = VGA[fg]
                if c in "─┌└":
                    d.line([mx, my, x * cw + cw, my], fill=col)
                if c in "─┐┘":
                    d.line([x * cw, my, mx, my], fill=col)
                if c in "│┌┐":
                    d.line([mx, my, mx, y * ch + ch], fill=col)
                if c in "│└┘":
                    d.line([mx, y * ch, mx, my], fill=col)
            elif c in "▶◄►":  # triangles: the font may lack them
                l, r, top, bot, mid = x * cw + 2, x * cw + cw - 2, y * ch + 5, y * ch + ch - 5, y * ch + ch // 2
                pts = [(l, top), (r, mid), (l, bot)] if c != "◄" else [(r, top), (l, mid), (r, bot)]
                d.polygon(pts, fill=VGA[fg])
            elif c != " ":
                d.text((x * cw, y * ch + 1), c, font=f, fill=VGA[fg])
    img.save(path)
    print(path)


def main():
    if os.geteuid() != 0:
        sys.exit("screens.py: as root (sudo): cleanlinux-setup without --dry-run")
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "docs/screens")
    binary = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "./cleanlinux-setup")
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        data = os.path.join(tmp, "data")
        t.make_data(data)
        t.make_medium(os.path.join(tmp, "medium"))
        res = os.path.join(tmp, "res")
        os.makedirs(res)
        with open(os.path.join(tmp, "installer"), "w") as f:
            f.write(t.INSTALLER.replace("sleep 0.3", "sleep 1") % {"out": res})
        os.chmod(os.path.join(tmp, "installer"), 0o755)
        # not a dry run (it says so on the screens): root, but the stand-in installer does nothing
        env = t.base_env(tmp, data, {"CLEANLINUX_SETUP_EFI": tmp})
        tui = t.Tui([binary], env, rows=30, cols=100)
        tui.screen = ColorScreen(30, 100)

        def shot(name, *texts):
            tui.wait_for(*texts)
            tui.settle(0.3)
            render(tui.screen, os.path.join(out, name + ".png"))

        shot("1-language", "Language · Язык")
        tui.send(t.DOWN, t.ENTER)
        shot("2-keyboard", "Раскладка клавиатуры")
        tui.send(t.ENTER)
        tui.wait_for("Часовой пояс")
        tui.type("eur")
        tui.send(t.ENTER)
        tui.type("mos")
        shot("3-timezone", "Поиск: mos", "Moscow")
        tui.send(t.ENTER)
        shot("4-disk", "/dev/vdb  QEMU HARDDISK")
        tui.send(t.ENTER)
        tui.wait_for("Компьютер и пользователь")
        tui.send(t.TAB)
        tui.type("anna")
        tui.send(t.TAB)
        tui.type("secret")
        tui.send(t.TAB)
        tui.type("secret")
        shot("5-user", "Компьютер и пользователь")
        tui.send(t.ENTER)
        shot("6-system", "Рабочий стол", "labwc")
        tui.send(t.ENTER)
        shot("7-summary", "Всё готово к установке")
        tui.send(t.ENTER, t.RIGHT, t.ENTER)
        shot("8-install", "Копирование системы: 50%")
        shot("9-done", "CleanLinux установлен")
        tui.send(t.RIGHT, t.RIGHT, t.ENTER)
        tui.finish()
    for f in ("/tmp/cleanlinux-install.log", "/run/cleanlinux-setup.exit"):
        if os.path.exists(f):
            os.unlink(f)


if __name__ == "__main__":
    main()
