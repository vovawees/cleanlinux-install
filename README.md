# cleanlinux-install

Установщик [CleanLinux](https://github.com/vovawees/CleanLinux): ставит работающую систему на диск одной
командой — как `pacstrap` + `genfstab` в Arch, но целиком и без выбора DE. Пакеты не собираются заново:
`burn clone` берёт из кэша ровно те сборки, что стоят в живой системе.

*English summary: the CleanLinux installer. Erases a disk (GPT, an EFI system partition and btrfs with the
subvolumes `@`, `@home`, `@snapshots`) or uses filesystems already mounted at `--target`; installs the very
package builds of the running system (`burn clone`), writes fstab by PARTUUID, the kernel command line,
hostname, passwords (yescrypt) and a user, enables dhcpcd (and iwd with Wi-Fi), puts the kernel and
systemd-boot into the ESP and adds an EFI boot entry. C17, libblkid, libxcrypt. MIT.*

## Использование

```sh
cleanlinux-install /dev/nvme0n1                      # спросит подтверждение и пароль root
cleanlinux-install --user anna --hostname deck /dev/sda
cleanlinux-install --dry-run /dev/sda                # показать план, ничего не меняя
cleanlinux-install --target /mnt                     # своя разметка: смонтировано в /mnt, ESP в /mnt/boot
```

С диском установщик **стирает его целиком**:

| Раздел | Размер | ФС | Куда |
|---|---|---|---|
| 1, EFI system partition | 512 MiB (`--esp-size`) | vfat | `/boot` |
| 2, корень (тип из Discoverable Partitions Spec для архитектуры) | остальное | btrfs, zstd | `@` → `/`, `@home` → `/home`, `@snapshots` → `/.snapshots` |

Снимки burn перед каждым `update` ложатся в `@snapshots`. Без диска (`--target`) установщик берёт то, что
уже смонтировано: корень и любые другие ФС ниже `--target`, ESP — vfat в `/boot`, `/efi` или `/boot/efi`.

## Что делает

1. Проверяет диск: целый диск, а не раздел; не только для чтения; не занят — ядро не даёт эксклюзивно
   открыть диск, если раздел смонтирован, это swap или его держит device mapper, так что диск живой системы
   не выбрать по ошибке.
2. Предлагает профиль железа: `burn profile detect` узнаёт устройство (DMI, device tree), и если это не тот профиль,
   под который собрана живая система (образ — `generic-x86_64`), спрашивает, взять ли его для новой системы
   (`burn update` потом пересоберёт её под это устройство); без терминала или с `--yes` только показывает
   подсказку, `--profile` выбирает сам.
3. Спрашивает подтверждение (`--yes` — без него) и пароли — до того, как что-то изменится.
4. Размечает (`sfdisk`, свои случайные PARTUUID), форматирует (`mkfs.vfat`, `mkfs.btrfs`), создаёт подтома
   ioctl-ом btrfs и монтирует новую систему в `--target`.
5. Пишет `/etc/kernel/cmdline` (`root=PARTUUID=… rootflags=subvol=@ rootwait ro` + `--cmdline`) — до пакетов,
   чтобы триггер ядра сразу написал верную запись загрузчика.
6. `burn -r /mnt -y clone /`: все пакеты живой системы (или `--packages` с зависимостями) из
   `/var/cache/burn/packages`, в порядке зависимостей, с триггерами (ldconfig, depmod, hwdb, sysusers, ядро в ESP).
7. Настраивает: `fstab` по PARTUUID, `hostname`, `burn.conf` живой системы (профиль — `--profile`), пароль root
   и пользователь в группах `wheel`, `audio`, `video` и `seat` — тех, что есть в новой системе (без logind звук
   и место seatd даются группами; `useradd -R`, хэш yescrypt из libxcrypt), сервисы dinit: `dhcpcd`, а при Wi-Fi —
   `iwd` и известные живой системе сети.
8. `cleanlinux-boot --root /mnt kernel sync`: ядро и systemd-boot в ESP, затем запись `CleanLinux` в NVRAM
   (efivarfs, первой в `BootOrder`); без неё прошивка всё равно найдёт `EFI/BOOT/BOOTX64.EFI`.
9. Отмонтирует (`--keep-mounted` — оставить).

Пароли берутся с терминала или из файла (`--root-password-file`, `--user-password-file`, `-` — stdin):
так установку можно автоматизировать. LUKS и LVM пока не поддерживаются — им нужен initramfs.

## Сборка

```sh
make && make check       # unit-тесты и тесты командной строки, без root
make install DESTDIR=… PREFIX=/usr
```

Нужны libblkid (util-linux) и libxcrypt; на устройстве — burn, cleanlinux-boot, util-linux (`sfdisk`),
dosfstools, btrfs-progs и shadow (`useradd`) — всё это есть в `base`. Настоящая установка проверяется в
CleanLinux: `tests/install.py` загружает образ в QEMU, ставит систему на второй диск и загружается с него.

## Лицензия

MIT
