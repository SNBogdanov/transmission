---
name: build
description: "Сборка Transmission daemon: Windows (CMake presets), Linux и QNAP (кросс-сборка на сервере). Use when building transmission-daemon."
---

# Сборка Transmission daemon

Проект: `C:\Users\SERGEYBO\Documents\Projects\Transmission`. Прочитай `AGENTS.md` и
`CLAUDE.md` (правила коммитов).

## Перед сборкой обязательно выясни

1. Какие цели собирать: `Windows`, `Linux`, `QNAP` (можно несколько).
2. Будет ли отладка и на какой машине. Если да — сначала собери только бинарник
   этой машины.

Не начинай сборку, пока не получишь оба ответа (если они не переданы явно).
Собери **все цели текущего этапа за один запуск**.

## Этапы и commit/push

- Сначала **Windows**; исправь ошибки Windows до Linux/QNAP.
- Linux/QNAP собираются **только из запушенного коммита**: сначала Windows,
  затем commit/push через `git-commit` (`/commit`), затем Linux+QNAP.
- Если в запросе Linux/QNAP, проверь, что HEAD запушен:
  `git rev-list --count "@{u}..HEAD"` = `0`. Иначе остановись и сообщи, что нужен
  commit/push; сам не коммить.
- Не синхронизируй незакоммиченное дерево на сервер (rsync/scp исходников запрещены).
- При поднятом VPN (CSM.27) не ходи по ssh на `transmission`/`qnap`.
- Не редактируй исходники, не делай commit/push.

## Windows

```powershell
cmake --preset Release
cmake --build --preset Release --target transmission-daemon
```

Артефакт: `build\Release\daemon\Release\transmission-daemon.exe`.

MSVC: `tr_error_code_t` — `int64_t` на Windows и `int` на POSIX. Не смешивай
`return ENOENT;` с `return err;` при `auto`; кастуй явно:
`return static_cast<tr_error_code_t>(ENOENT);`.

## Linux (на сервере `transmission`)

Перед сборкой обнови дерево до запушенного коммита:

```bash
ssh transmission 'cd /root/Transmission && git status --porcelain && git fetch origin && git merge --ff-only origin/main'
ssh transmission 'cd /root/Transmission && cmake --build build --target transmission-daemon -j2'
```

Артефакт: `/root/Transmission/build/daemon/transmission-daemon`.
Если дерево грязное или fast-forward невозможен — остановись и сообщи; не делай
`reset --hard`, не перезаписывай серверное дерево.

## QNAP (кросс-сборка на `transmission`, ARMv5)

```bash
ssh transmission 'cd /root/Transmission && cmake --build build-qnap --target transmission-daemon -j1'
```

Артефакт: `/root/Transmission/build-qnap/daemon/transmission-daemon`.
Скрипта `/tmp/qnap-build-j1.sh` нет — собирай существующую `build-qnap`.

## Уроки предыдущих сборок

- Перед сборкой проверь, что изменённые C++-файлы не конфликтуют с новыми
  quark-ключами: `libtransmission/quark.h` и `quark.cc` должны быть
  отсортированы по строке. Иначе Windows-сборка падает на
  `static_assert(quarks_are_sorted())`; добавляй ключ в оба файла в одну и ту же
  алфавитную позицию.
- После Windows-сборки main-сессия коммитит только предназначенные файлы.
  В рабочем дереве могут быть чужие/временные изменения (`scripts/runner/*`,
  `AGENTS.md`, `.tmp-srtest/`) — они не должны попасть в коммит.
- Linux/QNAP собирай только из запушенного `HEAD`. На сервере проверяй чистоту
  именно отслеживаемых файлов; грязные submodule-деревья и untracked backups
  сами по себе не блокируют fast-forward, но reset/clean делать нельзя.
- Сразу после каждой сборки фиксируй путь, размер, mtime, MD5 и SHA256 артефакта:
  QNAP-бинарник перезаписывается следующим `build-qnap`, и старый хэш теряется.
- Если сборка прервалась, не запускай её повторно вслепую: проверь `HEAD` на
  сервере, mtime/хэши артефактов и наличие процессов `cmake`/`g++`; дособери
  только недостающую цель.

## Отчёт

Какие артефакты собраны, пути, результат, предупреждения, MD5/SHA256.
