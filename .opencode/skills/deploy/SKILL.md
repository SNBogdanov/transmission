---
name: deploy
description: "Деплой transmission-daemon на transmission/qnap/sergeybo02/samsung-pc: stop, backup, copy, start, проверка. Use when deploying a built transmission-daemon."
---

# Деплой transmission-daemon

Деплой выполняет только субагент `transmission-deploy`. Прочитай `AGENTS.md`
(раздел Deployment) целиком. Main-сессия не запускает `ssh`/копирование сама.

## Перед деплоем обязательно выясни

1. Какие цели обновлять: `transmission`, `qnap`, `sergeybo02`, `samsung-pc`.
2. Это отладочный деплой на одну машину или полный rollout. При отладке — только
   выбранная цель; остальные после подтверждения успешного теста.

Затем задеплой **все выбранные цели за один запуск**, не возвращаясь после первой.
При поднятом VPN (CSM.27) локальная сеть недоступна — отложи.

## Общие правила

- Для каждой цели: проверь службу → stop → backup с timestamp → copy → start →
  проверка.
- Не копируй поверх работающего процесса; для служб — stop/start, не rename.
- **Не держи больше 2 backup на бинарник.** После успешного деплоя и проверки
  удали более старые, оставь две самые новые timestamp-копии.
- Никогда не удаляй активный бинарник и непроверенный backup.
- Не выводи и не хардкодь пароли; для `samsung-pc` — `machine samsung-pc` из `_netrc`.

## 1. transmission server

SSH alias `transmission`; цель `/usr/local/bin/transmission-daemon`.

```bash
ts=$(date +%Y%m%d-%H%M%S)
systemctl stop transmission-daemon watchdog
cp -p /usr/local/bin/transmission-daemon "/usr/local/bin/transmission-daemon.$ts"
# copy the new binary to /usr/local/bin/transmission-daemon
systemctl start transmission-daemon watchdog
```

## 2. QNAP server

SSH alias `qnap` с рабочей станции (admin, `armv5tel`). Сервер `transmission` не
может ssh на `qnap` неинтерактивно — копируй через рабочую станцию:

```powershell
scp transmission:/root/Transmission/build-qnap/daemon/transmission-daemon .
scp .\transmission-daemon qnap:/share/MD0_DATA/.qpkg/Transmission/bin/transmission-daemon.new
```

Не использовать `/tmp` на QNAP (32 MB). QNAP — 256 MB RAM и медленный: увеличенные
таймауты для `ssh`/`scp`/команд.

```sh
/etc/init.d/transmission.sh stop
cd /share/MD0_DATA/.qpkg/Transmission/bin
ts=$(date +%Y%m%d-%H%M%S)
cp -p transmission-daemon "transmission-daemon.$ts"
mv -f transmission-daemon.new transmission-daemon
chmod 755 transmission-daemon
/etc/init.d/transmission.sh start
```

Проверка: `ps | grep transmission-daemon`, `md5sum`, симлинк `/usr/bin/transmission-daemon`.

## 3. sergeybo02 (локально)

Агент не elevated. Скрипт `.ps1` в `C:\Windows\SERVIC~1\LOCALS~1\AppData\Local\Temp\opencode\`
с `Start-Transcript`, запуск elevated:

```powershell
Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList `
  '-NoProfile','-ExecutionPolicy','Bypass','-File','<script.ps1>'
```

Скрипт: stop `Transmission` (дождись `Stopped`) → backup
`C:\Program Files\Transmission\transmission-daemon.exe` в `.exe.<yyyyMMdd-HHmmss>`
→ copy `build\Release\daemon\Release\transmission-daemon.exe` → start (дождись `Running`).
Проверь `Get-Service Transmission` и MD5 установленного бинарника.

## 4. samsung-pc (удалённый Windows)

- DNS `samsung-pc` = `192.168.1.184`; SMB 445 открыт, SSH 22 закрыт.
- PsExec: `C:\Users\SERGEYBO\Downloads\SysinternalsSuite\PsExec64.exe`.
- Учётка — `machine samsung-pc` из `_netrc`; запуск как SYSTEM с `-s`.
- Копирование через `\\samsung-pc\C$` (сессия SMB уже есть; `net use` не нужен,
  возможна ошибка 1219).

```powershell
Copy-Item .\build\Release\daemon\Release\transmission-daemon.exe `
  '\\samsung-pc\C$\Program Files\Transmission\transmission-daemon.exe.new'
Copy-Item .\deploy-samsung.ps1 '\\samsung-pc\C$\Windows\Temp\deploy-samsung.ps1'
& 'C:\Users\SERGEYBO\Downloads\SysinternalsSuite\PsExec64.exe' \\samsung-pc `
  -accepteula -nobanner -u <from _netrc> -p <from _netrc> -s `
  C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe `
  -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\deploy-samsung.ps1
```

Удалённый скрипт: stop `Transmission` → backup `.exe.<yyyyMMdd-HHmmss>` → move
`.exe.new` → start → лог `C:\Windows\Temp\deploy-samsung.log`. Прочитай лог и
сверь MD5.

## Уроки предыдущих деплоев

- **QNAP:** не выполняй многострочный sh-код через PowerShell/SSH одной строкой —
  `$(date ...)` и `$?` раскрываются локально, из-за чего backup может не создаться.
  Передавай удалённый скрипт через heredoc/base64 или `ssh ... bash -s`.
- **QNAP:** backup обязан быть создан и проверен по MD5 **до** `mv` нового бинарника.
  Если backup не подтверждён — остановись, не заменяй рабочий файл.
- **QNAP:** `transmission.sh stop` может вернуть ненулевой код, хотя процессов уже нет.
  Проверяй фактическое отсутствие `transmission-daemon`, а не только rc.
- **QNAP:** busybox может не поддерживать `cut -f1`; для проверки используй
  `md5sum`/`ps`, а не сложные пайплайны. После старта проверь процесс, symlink и MD5.
- **samsung-pc:** используй `\\192.168.1.184\C$`, если `\\samsung-pc\C$` даёт
  ошибку 1219 из-за существующих SMB-сессий. PsExec может писать progress-строку
  в stderr; при `ErrorActionPreference=Stop` это выглядит как ошибка. Сверяй
  `C:\Windows\Temp\deploy-samsung.log` и MD5, не делай повторный деплой вслепую.
- **sergeybo02:** служба может быть уже Stopped до деплоя; всё равно дождись
  `Stopped`, сделай backup, скопируй и дождись `Running`; проверь MD5.
- **transmission:** останавливай `watchdog` и `transmission-daemon`, делай backup
  с timestamp и проверяй его MD5 перед заменой. Оставляй 2 timestamped backup.
- **Все цели:** после деплоя сверяй установленный MD5 с артефактом и фиксируй
  в отчёте backup-файл; не удаляй backup, пока установка не проверена.

## Отчёт

Что остановлено, backup-файлы, что скопировано, проверка (MD5/ps/служба), что
осталось сделать.
