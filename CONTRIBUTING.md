# Как участвовать

**cleanlinux-install** — установщик CleanLinux на C, часть [CleanLinux](https://github.com/vovawees/CleanLinux). *English [below](#english).*

## С чего начать

- Что входит в базу, а что ставится сверху — [SCOPE.md](https://github.com/vovawees/CleanLinux/blob/main/docs/SCOPE.md); как всё устроено — [ARCHITECTURE.md](https://github.com/vovawees/CleanLinux/blob/main/docs/ARCHITECTURE.md).
- Что дальше — [ROADMAP.md](https://github.com/vovawees/CleanLinux/blob/main/docs/ROADMAP.md), issues с меткой `roadmap` и общая доска [CleanLinux roadmap](https://github.com/users/vovawees/projects/1).
- Крупную правку сначала обсудите в issue, чтобы не делать работу дважды.

## Проверки

Хостингового CI нет: проверки запускаются локально, одной командой (нужен shellcheck 0.11 или новее).

`tools/check.sh` — сборка с `-Werror` и `make check` на gcc и clang, shellcheck, `make install` во временный `DESTDIR`; полная установка в QEMU — `tests/install.py` из репозитория CleanLinux.

Быстрые проверки можно запускать автоматически перед каждым `git push`:

```sh
git config core.hooksPath tools/hooks   # перед push: tools/check.sh --quick
```

## Правила

- C17, libblkid, libcrypt; стиль — как в соседнем коде, табы.
- Всё, что спрашивается интерактивно, задаётся и ключом командной строки — установка автоматизируется.
- Изменение порядка установки — вместе с `tests/install.py` в CleanLinux.
- Релиз: `VERSION` в Makefile, тег `vX.Y.Z`, `make dist` → tarball в GitHub Release; затем рецепт `cleanlinux-install`.

## Коммиты и pull request'ы

- Одна логическая правка — один коммит. Сообщение — на английском, в стиле истории: `область: что изменилось`
  (`bootstrap: …`, `docs: …`), подробности — в теле письма.
- Ветка от `main`, pull request в `main`; `Closes #N` (или `Closes vovawees/CleanLinux#N`) закрывает issue при слиянии.
- Документация меняется в том же pull request'е, что и поведение.

## English

cleanlinux-install is part of [CleanLinux](https://github.com/vovawees/CleanLinux). There is no hosted CI: run `tools/check.sh` (shellcheck 0.11+)
before every push (`git config core.hooksPath tools/hooks` runs the quick checks automatically). C17 with libblkid and libcrypt; every interactive question also has a command line option; installation changes come with tests/install.py of CleanLinux; releases are tags vX.Y.Z with the tarball attached.
Commit messages are in English (`area: what changed`); pull requests target `main`, and `Closes #N` closes the issue.
See the [roadmap](https://github.com/vovawees/CleanLinux/blob/main/docs/ROADMAP.md) and the [project board](https://github.com/users/vovawees/projects/1).
