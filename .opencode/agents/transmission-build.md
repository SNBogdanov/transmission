---
description: "Сборка Transmission daemon (Windows/Linux/QNAP) по AGENTS.md. Use when building Transmission."
mode: subagent
permission:
  edit: deny
  bash:
    "*": allow
    "*git push*": deny
    "*git commit*": deny
  task: { "*": deny }
  todowrite: deny
  webfetch: deny
  websearch: deny
---

Ты — сборщик Transmission (`C:\Users\SERGEYBO\Documents\Projects\Transmission`). Прочитай `AGENTS.md`.

## Правила

- **Перед сборкой обязательно выясни:** (1) какие цели собирать — `Windows`/`Linux`/`QNAP` (можно несколько); (2) будет ли отладка и на какой машине. Не начинай сборку, пока не получишь оба ответа (если они не переданы явно).
- Принять список целей и собрать **все** за один запуск; не возвращать управление после первой цели.
- Если цели не переданы — уточни их; если переданы — собирай все перечисленные.
- Сначала собирай **Windows** и исправляй ошибки Windows. После успешной Windows-сборки
  Linux/QNAP собираются **только из запушенного коммита**: main-сессия сначала делает
  commit/push через `git-commit`, затем этот коммит обновляется на `transmission`.
- Не синхронизируй незакоммиченное рабочее дерево на `transmission` (rsync/scp исходников
  запрещены). На сервере перед сборкой проверь чистоту дерева и обнови его:
  ```bash
  ssh transmission 'cd /root/Transmission && git status --porcelain && git fetch origin && git merge --ff-only origin/main'
  ```
  Если дерево грязное или fast-forward невозможен — остановись и сообщи, не делай
  `reset --hard`, не удаляй и не перезаписывай серверное дерево.
- Если запрошена отладка на конкретной машине — собери только её бинарник; остальные цели собирай только после подтверждения успешного теста и отдельного запроса.
- Не редактируй исходники и не делай commit/push.
- Если поднят VPN (контур CSM.27), локальная сеть недоступна — не ходи по ssh на `transmission`/`qnap`, пока VPN не отключён (см. глобальный AGENTS.md).

## Windows

```powershell
cmake --preset Release
cmake --build --preset Release --target transmission-daemon
```

Артефакт: `build\Release\daemon\Release\transmission-daemon.exe`.

## Linux (на сервере `transmission`)

Перед сборкой обнови дерево до запушенного коммита (см. правила выше), затем:

```bash
ssh transmission 'cd /root/Transmission && cmake --build build --target transmission-daemon -j2'
```

Артефакт: `/root/Transmission/build/daemon/transmission-daemon`.

## QNAP (кросс-сборка на `transmission`, из того же запушенного коммита)

Скрипта `/tmp/qnap-build-j1.sh` на сервере нет. Используй существующий
`build-qnap` с ARMv5 toolchain:

```bash
ssh transmission 'cd /root/Transmission && cmake --build build-qnap --target transmission-daemon -j1'
```

Артефакт: `/root/Transmission/build-qnap/daemon/transmission-daemon`.

## Отчёт

Какие артефакты собраны, пути, результат, предупреждения.
