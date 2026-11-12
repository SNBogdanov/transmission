---
name: git
description: "Git Transmission: remotes origin/fork/server, ветка main, Conventional Commits, push только после ревью. Use when committing, pushing or inspecting git history in Transmission."
---

# Git — Transmission

Репозиторий: `C:\Users\SERGEYBO\Documents\Projects\Transmission`.

## Remotes и ветка

- `origin` = https://github.com/transmission/transmission (fetch, upstream) и
  https://github.com/SNBogdanov/transmission.git (push, форк).
- `fork` = https://github.com/SNBogdanov/transmission.git.
- `server` = `ssh://root@transmission/root/Transmission` (сборочное дерево Linux/QNAP).
- Рабочая ветка: `main` (upstream `origin/main`).

## Жёсткие правила

- Коммит и пуш — только по явной просьбе и только через субагент `git-commit`
  (`/commit`). Main-сессия не делает `git add/commit/push` сама.
- **Linux/QNAP собираются только из запушенного коммита.** После успешной
  Windows-сборки сначала commit/push, затем Linux+QNAP. Просьба пользователя
  собрать Linux/QNAP — это и есть разрешение на push.
- Не делать force, amend, `--no-verify`; не менять git config.
- `/root/Transmission` на сервере не перезаписывать: только
  `git status --porcelain && git fetch origin && git merge --ff-only origin/main`.
  Грязное дерево или невозможный fast-forward — стоп и отчёт.
- Push в CSM.27 — online-действие: сначала подтверди VPN.

## Сообщения коммитов (CLAUDE.md)

- Conventional Commits: `type: short description`, 50/72 best effort, ясность
  важнее лимитов.
- Subject покрывает всё изменение; если не получается — раздели коммит.
- Body — только «почему»; не пересказывай subject/дифф и процесс.

## Push и PR

- Не пуш и не открывай PR, пока человек не просмотрел изменения и явно не
  подтвердил. Локальный коммит + отчёт допустимы.
- Одобрение действует только на один push; для следующих изменений — новое.
- Перед коммитом проверь diff на секреты и артефакты `build/`; не стейджить их.

## Уроки предыдущих коммитов

- В рабочем дереве часто остаются посторонние изменения и untracked-файлы
  (`scripts/runner/*`, `AGENTS.md`, `.opencode/*`, `.tmp-srtest/`). Стейджи
  **только явные пути**, не используй `git add -A`.
- `.tmp-srtest/` может содержать тестовые netrc/JSON — никогда не коммить его.
- Для сборок Linux/QNAP коммит должен содержать именно C++/docs/CHANGELOG,
  необходимые для сборки; не смешивай с ним незавершённые скрипты.
- После push проверь `git status` и `git log --oneline -1`; в отчёте укажи,
  какие файлы вошли в коммит, а какие остались вне него.
