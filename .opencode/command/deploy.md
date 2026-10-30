---
description: "Задеплоить transmission-daemon на transmission/qnap/sergeybo02/samsung-pc."
agent: transmission-deploy
---
Задеплой transmission-daemon. Цели: $ARGUMENTS

Спроси цели и подтверждение. Для каждой: stop → backup с timestamp → copy → start → проверка. QNAP: без `/tmp`, через рабочую станцию. `sergeybo02`: elevated `.ps1`. `samsung-pc`: PsExec64 `-s`, учётка из `_netrc`. При поднятом VPN локальные хосты не трогать.
