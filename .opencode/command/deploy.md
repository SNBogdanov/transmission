---
description: "Задеплоить transmission-daemon на transmission/qnap/sergeybo02/samsung-pc."
agent: transmission-deploy
---
Задеплой transmission-daemon. Цели: $ARGUMENTS

**Перед деплоем обязательно уточни:**
1. какие цели обновлять — `transmission`, `qnap`, `sergeybo02`, `samsung-pc`;
2. это отладочный деплой на одну машину или полный rollout. Если отладочный — только выбранная цель; остальные после подтверждения теста.

Не начинай деплой, пока не получишь ответы (если они не переданы явно в `$ARGUMENTS`). Затем задеплой **все выбранные цели за один запуск**, не возвращаясь после первой. Спроси подтверждение перед деплоем. Для каждой: stop → backup с timestamp → copy → start → проверка. QNAP: без `/tmp`, через рабочую станцию. `sergeybo02`: elevated `.ps1`. `samsung-pc`: PsExec64 `-s`, учётка из `_netrc`. При поднятом VPN локальные хосты не трогать.
