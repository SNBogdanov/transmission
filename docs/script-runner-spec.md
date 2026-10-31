# Script Runner для Transmission daemon

Версия документа: 1.7  
Статус: проект технического задания  
Область: только скрипты, исполняемые на хосте демона  
Поддерживаемые платформы: Linux (включая QNAP) и Windows

## 1. Назначение

Добавить в демон Transmission механизм выполнения административно разрешённых внешних скриптов по запросу RPC-клиента.

Механизм должен позволять TransGUI и другим клиентам:

- получать список скриптов, доступных на конкретном демоне;
- получать описание параметров скрипта: типы, признак массива, значения по умолчанию и ограничения;
- запускать скрипт для выбранных торрентов;
- запускать скрипт для выбранных файлов торрента;
- передавать значения runtime-параметров, объявленных для скрипта, при каждом запуске;
- хранить и изменять параметры `storage=host` отдельно для каждого хоста демона;
- управлять параметрами `storage=host` в окне «Параметры Transmission» клиента TransGUI;
- запускать оркестраторные скрипты, которые обращаются к нескольким внешним демонам;
- получать каталог разрешённых внешних хостов и выбирать только их идентификаторы;
- перезапускать службу демона отдельной административной операцией в контексте `system`;
- получать состояние выполнения, код возврата и ограниченный вывод;
- отменять выполняющийся скрипт;
- не передавать клиенту пути к исполняемым файлам и не разрешать произвольные команды.

Клиент передаёт идентификаторы выбранных объектов (хеши торрентов и индексы файлов), значения runtime-параметров, объявленных в descriptor скрипта, и идентификаторы внешних хостов из разрешённого каталога. Параметры `storage=host` клиент не передаёт в `script_run`: демон хранит их отдельно для каждого хоста и подставляет при формировании payload. Демон сам разрешает выбор в данные торрентов и файлов, проверяет принадлежность, типы, ограничения параметров и допустимость внешних хостов, затем формирует payload для скрипта. Клиент не может передать путь к файлу, URL внешнего RPC, netrc machine или параметр, не объявленный для скрипта.

Скрипты выполняются на том же хосте, где работает демон, от имени пользователя демона. Запуск скриптов на другом хосте, на клиенте или через SSH не входит в данный механизм.

В версии `v1` скрипты получают URL локального RPC и путь к защищённому netrc-файлу. Учётные данные RPC хранятся только в netrc; payload не содержит логинов, паролей, cookie или иных секретов. Скрипты могут выполнять RPC-действия, включая изменение содержимого, в пределах прав пользователя RPC.

Скрипты считаются полностью доверенным административным кодом. Runner не является sandbox: скрипт работает от имени пользователя демона и с полным RPC-доступом может делать всё, что может сам демон. Не устанавливайте недоверенные скрипты.

Оркестраторные скрипты выполняются на хосте демона и сами обращаются к внешним RPC. Runner не проксирует внешние запросы и не передаёт клиенту URL или учётные данные внешних хостов; он только разрешает выбранные идентификаторы из административного каталога в данные для скрипта.

Перезапуск службы демона является отдельной административной операцией. Runner не повышает свои права и не выполняет произвольные команды: он запускает заранее настроенный helper, который service manager или администратор наделяет необходимыми правами.

## 2. Термины

- **Runner** — подсистема демона, которая загружает конфигурацию скриптов, проверяет запросы и запускает процессы.
- **Descriptor** — публичное описание скрипта, которое демон передаёт клиентам.
- **Script ID** — стабильный идентификатор скрипта в конфигурации.
- **Job** — один запуск скрипта.
- **Request Payload** — JSON-объект, который клиент передаёт в `script_run` и который содержит селекторы и значения runtime-параметров скрипта.
- **Execution Payload** — JSON-объект, который демон формирует и передаёт скрипту в stdin; содержит разрешённые данные торрентов и файлов и разрешённые значения runtime-параметров и параметров `storage=host`.
- **Context** — значение `context` (`torrent`, `file` или `system`), определяющее набор данных и меню клиента.
- **Selection** — набор выбранных клиентом торрентов и файлов.
- **Script Parameter** — типизированный параметр, объявленный в конфигурации скрипта; клиент получает его описание в descriptor.
- **Runtime Parameter** — параметр со `storage: "runtime"`; клиент передаёт его значение в Request Payload при каждом запуске. Значение не сохраняется демоном.
- **Stored Host Parameter** — параметр со `storage: "host"`; значение сохраняется на демоне отдельно для каждого хоста и не передаётся клиентом в `script_run`.
- **Host-Target Parameter** — параметр с `type: "host"`; его значением является `id` внешнего хоста. Это ортогонально `storage`.
- **Stored Host Parameter Values** — файл `script-runner-values.json` с сохранёнными значениями параметров `storage=host`; управляется через RPC и окно «Параметры Transmission».
- **External Host** — административно разрешённый внешний демон Transmission; клиент выбирает его `id`, а скрипт получает URL и netrc machine для самостоятельного RPC-доступа.
- **Restart Helper** — административный исполняемый файл, который выполняет перезапуск службы демона с необходимыми правами.
- **Job Token** — секретный токен, который возвращается создавшему клиенту и требуется для `script_status` и `script_cancel`.
- **Netrc** — защищённый файл с учётными данными для доступа к RPC демона, в том числе к внешним хостам.

## 3. Конфигурация

### 3.1. Расположение

Конфигурация хранится отдельно от `settings.json`:

```text
<config-dir>/script-runner.json
```

Файл читается при старте демона. На Linux изменения применяются после перезапуска демона или после `SIGHUP` / `systemctl reload transmission-daemon`. На Windows изменения применяются только после перезапуска службы.

Сохранённые значения параметров `storage=host` хранятся отдельно:

```text
<config-dir>/script-runner-values.json
```

Этот файл необязателен, не влияет на включение runner и описан в разделе 3.5.1.

Если `script-runner.json` отсутствует, runner выключен; feature `script_runner/v1` отсутствует. Это не считается фатальной ошибкой демона.

Загрузка конфигурации выполняется атомарно: новый файл сначала полностью читается и валидируется, и только затем заменяет текущую конфигурацию. Уже запущенные и ожидающие jobs сохраняют снимок конфигурации, с которым были созданы; новые jobs используют новую конфигурацию.

Если новый файл невалиден, runner выключается (fail closed): новые jobs не принимаются, ожидающие jobs переводятся в `canceled`, запущенные jobs продолжают выполняться до завершения, feature `script_runner/v1` исчезает, ошибка пишется в лог. Последняя рабочая конфигурация не сохраняется.

Файл и каталог скриптов должны быть доступны на запись только администратору, системе и пользователю демона. На POSIX файл не должен быть group-writable или world-writable. На Windows ACL не должен разрешать запись `Everyone`, `Users` или `Authenticated Users`. Если проверить права невозможно, runner выключается.

`script-runner-values.json` и его каталог должны быть доступны на запись только администратору, системе и пользователю демона. Файл не является административной конфигурацией скриптов: клиент RPC не может изменять `script-runner.json`, но может изменять значения `storage=host` через `script_settings_set` в пределах объявленных параметров `storage=host`.

Клиент RPC не может изменять административную конфигурацию `script-runner.json`.

Runner загружается только на Linux и Windows. На macOS, Haiku и других ОС feature `script_runner/v1` отсутствует.

### 3.2. Формат

```json
{
  "schema": "transgui-script-runner/v1",
  "enabled": true,
  "max_concurrent": 1,
  "queue_limit": 16,
  "queue_limit_per_client": 4,
  "max_concurrent_per_client": 1,
  "job_retention_seconds": 3600,
  "max_retained_jobs": 64,
  "max_total_output_bytes": 67108864,
  "max_output_bytes_default": 1048576,
  "max_selection_items": 10000,
  "max_payload_bytes": 8388608,
  "rpc_netrc_file": "/etc/transmission/script-rpc.netrc",
  "external_hosts": [
    {
      "id": "qnap",
      "name": "QNAP",
      "description": "QNAP Transmission",
      "rpc_url": "http://192.168.1.10:9091/transmission/rpc",
      "netrc_machine": "qnap"
    },
    {
      "id": "samsung-pc",
      "name": "samsung-pc",
      "description": "Windows Transmission",
      "rpc_url": "http://192.168.1.184:9091/transmission/rpc",
      "netrc_machine": "samsung-pc"
    }
  ],
  "restart_daemon": {
    "enabled": false,
    "helper": "/usr/local/sbin/transmission-restart",
    "args": [],
    "cwd": "/",
    "timeout_seconds": 30,
    "delay_seconds": 2,
    "cooldown_seconds": 60
  },
  "scripts": [
    {
      "id": "torrent-priority",
      "name": "Set file priorities",
      "description": "Set file priorities for selected torrents",
      "group": "Torrents",
      "order": 10,
      "enabled": true,
      "platform": "any",
      "contexts": ["torrent"],
      "requires_selection": true,
      "confirm": true,
      "parameters": [
        {"key": "priority", "name": "Priority", "type": "integer", "storage": "runtime", "default": 0, "min": -1, "max": 1},
        {"key": "dry_run", "name": "Dry run", "type": "boolean", "storage": "runtime", "default": false}
      ],
      "payload": "transgui.script/v1",
      "stdin": "json",
      "executable": "/usr/bin/php",
      "args": ["/opt/transgui/scripts/priority.php"],
      "cwd": "/opt/transgui/scripts",
      "timeout_seconds": 300,
      "max_output_bytes": 1048576
    },
    {
      "id": "file-delete",
      "name": "Delete selected files",
      "description": "Delete selected files and mark them unwanted",
      "group": "Files",
      "order": 20,
      "enabled": true,
      "platform": "any",
      "contexts": ["file"],
      "requires_selection": true,
      "confirm": true,
      "parameters": [
        {"key": "delete_data", "name": "Delete downloaded data", "type": "boolean", "storage": "runtime", "default": false},
        {"key": "keep_extensions", "name": "Keep extensions", "type": "string", "storage": "runtime", "array": true}
      ],
      "payload": "transgui.script/v1",
      "stdin": "json",
      "executable": "/usr/bin/php",
      "args": ["/opt/transgui/scripts/delete-file.php"],
      "cwd": "/opt/transgui/scripts",
      "timeout_seconds": 120,
      "max_output_bytes": 262144
    },
    {
      "id": "copy-to-host",
      "name": "Copy to external host",
      "description": "Copy selected torrents to another daemon",
      "group": "Orchestration",
      "order": 30,
      "enabled": true,
      "platform": "linux",
      "contexts": ["torrent"],
      "requires_selection": true,
      "confirm": true,
      "parameters": [
        {"key": "target", "name": "Target host", "type": "host", "storage": "runtime", "required": true, "hosts": ["qnap", "samsung-pc"]},
        {"key": "dry_run", "name": "Dry run", "type": "boolean", "storage": "runtime", "default": false},
        {"key": "copy_retries", "name": "Copy retries", "type": "integer", "storage": "host", "default": 3, "min": 0, "max": 10}
      ],
      "payload": "transgui.script/v1",
      "stdin": "json",
      "executable": "/usr/bin/php",
      "args": ["/opt/transgui/scripts/copy-to-host.php"],
      "cwd": "/opt/transgui/scripts",
      "timeout_seconds": 3600,
      "max_output_bytes": 1048576
    },
    {
      "id": "archive-cleanup",
      "name": "Cleanup archive",
      "description": "Delete old files from the archive",
      "group": "System",
      "order": 40,
      "enabled": true,
      "platform": "any",
      "contexts": ["system"],
      "requires_selection": false,
      "confirm": true,
      "parameters": [
        {"key": "archive_dir", "name": "Archive directory", "type": "string", "storage": "host", "required": true},
        {"key": "keep_days", "name": "Keep days", "type": "integer", "storage": "host", "default": 30, "min": 1, "max": 365},
        {"key": "dry_run", "name": "Dry run", "type": "boolean", "storage": "runtime", "default": false}
      ],
      "payload": "transgui.script/v1",
      "stdin": "json",
      "executable": "/usr/bin/php",
      "args": ["/opt/transgui/scripts/archive-cleanup.php"],
      "cwd": "/opt/transgui/scripts",
      "timeout_seconds": 3600,
      "max_output_bytes": 1048576
    }
  ]
}
```

### 3.3. Поля верхнего уровня

| Поле | Тип | Обязательно | По умолчанию | Описание |
|---|---|---:|---:|---|
| `schema` | string | да | — | Всегда `transgui-script-runner/v1` |
| `enabled` | boolean | нет | `true` | Если `false`, runner не анонсируется |
| `max_concurrent` | integer | нет | 1 | Максимум одновременно выполняемых скриптов; допустимо 1..16 |
| `queue_limit` | integer | нет | 16 | Максимум ожидающих jobs; допустимо 1..256 |
| `queue_limit_per_client` | integer | нет | 4 | Максимум ожидающих jobs на одного клиента; допустимо 1..`queue_limit` |
| `max_concurrent_per_client` | integer | нет | 1 | Максимум одновременно выполняемых jobs на одного клиента; 0 — без отдельного лимита; допустимо 0..`max_concurrent` |
| `job_retention_seconds` | integer | нет | 3600 | Сколько хранить завершённые jobs; допустимо 1..86400 |
| `max_retained_jobs` | integer | нет | 64 | Максимум завершённых jobs; допустимо 1..1024 |
| `max_total_output_bytes` | integer | нет | 67108864 | Суммарный лимит сохранённого вывода завершённых jobs; допустимо 1..1073741824 |
| `max_output_bytes_default` | integer | нет | 1048576 | Лимит вывода по умолчанию на каждый поток; допустимо 1..67108864 |
| `max_selection_items` | integer | нет | 10000 | Максимум хешей и файлов в одном выборе; допустимо 1..100000 |
| `max_payload_bytes` | integer | нет | 8388608 | Максимум размера Request Payload и Execution Payload; допустимо 1024..67108864 |
| `rpc_netrc_file` | string | нет | `null` | Абсолютный путь к защищённому netrc для локального и внешнего RPC; если не задан, скрипты используют собственные креды |
| `external_hosts` | array | нет | `[]` | Каталог разрешённых внешних хостов; 0..64 элементов |
| `restart_daemon` | object | нет | `{}` | Настройки административного перезапуска службы; по умолчанию выключено |
| `scripts` | array | да | — | Список описаний скриптов; допустимо 0..256 элементов |

`scripts` не содержат значений параметров `storage=host`. Значения `storage=host` хранятся отдельно в `script-runner-values.json` и не являются полем верхнего уровня `script-runner.json`.

`max_total_output_bytes` применяется к сохранённому выводу завершённых jobs. Вывод активных jobs ограничен `max_concurrent` и `max_output_bytes` каждого скрипта. При загрузке конфигурации проверяется, что `max_total_output_bytes` не меньше удвоенного максимального эффективного `max_output_bytes` среди скриптов, доступных на текущей платформе, чтобы вывод одного job мог быть сохранён целиком.

`queue_limit_per_client` и `max_concurrent_per_client` применяются к `client_id`, переданному в `script_run`. Если `client_id` не задан, используется общий bucket `default`. `client_id` не является средством авторизации и может быть подделан; он нужен только для fairness и защиты от случайного monopolization.

### 3.4. Поля скрипта

| Поле | Тип | Обязательно | По умолчанию | Описание |
|---|---|---:|---:|---|
| `id` | string | да | — | Уникальный идентификатор: `[a-z0-9][a-z0-9._-]{0,63}` |
| `name` | string | да | — | Отображаемое имя, непустое, до 128 символов |
| `description` | string | нет | `""` | Описание для клиента, до 1024 символов |
| `group` | string | нет | `""` | Группа в меню, до 64 символов |
| `order` | integer | нет | 0 | Порядок сортировки |
| `enabled` | boolean | нет | `true` | Выключенный скрипт не анонсируется и не запускается |
| `platform` | string | нет | `any` | `any`, `linux` или `windows`; `any` соответствует Linux и Windows |
| `contexts` | array | да | — | Непустой набор уникальных значений из `torrent`, `file`, `system`; `system` не комбинируется с другими |
| `requires_selection` | boolean | нет | `true` | Требуется непустой выбор |
| `confirm` | boolean | нет | `false` | Показывать подтверждение в клиенте |
| `parameters` | array | нет | `[]` | Описания runtime-параметров и параметров `storage=host` скрипта; до 64 элементов |
| `payload` | string | да | — | Версия контракта данных, сейчас `transgui.script/v1` |
| `stdin` | string | да | — | Сейчас поддерживается только `json` |
| `executable` | string | да | — | Абсолютный путь к интерпретатору или исполняемому файлу |
| `args` | array | нет | `[]` | Фиксированный массив аргументов; shell не используется |
| `cwd` | string | да | — | Абсолютный существующий рабочий каталог |
| `env` | object | нет | `{}` | Дополнительные переменные окружения |
| `timeout_seconds` | integer | нет | 300 | Таймаут выполнения; допустимо 1..86400 |
| `max_output_bytes` | integer | нет | `max_output_bytes_default` | Лимит отдельно для stdout и отдельно для stderr; допустимо 1..67108864 |

Запрещено использовать `executable` и `args` для запуска shell-команд. Все элементы `args` передаются процессу как отдельные аргументы.

Секреты не должны храниться в `args` или `env`, потому что они могут быть видны в списке процессов или в окружении. Для секретов используется системный secret store, SSH-agent или отдельный конфигурационный файл с ограниченными правами.

### 3.5. Параметры скрипта

`parameters` — необязательный массив описаний параметров. Каждый параметр имеет `storage`:

- `runtime` — значение запрашивается клиентом при каждом запуске и передаётся в `payload.parameters`; демон не сохраняет его;
- `host` — значение сохраняется на демоне отдельно для каждого хоста и не передаётся клиентом в `script_run`; клиент изменяет его через `script_settings_set` в окне «Параметры Transmission».

Параметры не являются секретами: они попадают в payload или в файл значений `storage=host` и могут быть видны в RPC-запросах. Для паролей и токенов используйте netrc или системный secret store.

| Поле | Тип | Обязательно | По умолчанию | Описание |
|---|---|---:|---:|---|
| `key` | string | да | — | Уникальный ключ параметра: `[a-z][a-z0-9_]{0,63}` |
| `name` | string | да | — | Отображаемое имя, непустое, до 128 символов |
| `description` | string | нет | `""` | Описание для клиента, до 1024 символов |
| `type` | string | да | — | `string`, `integer`, `number`, `boolean` или `host` |
| `storage` | string | нет | `runtime` | `runtime` или `host` |
| `array` | boolean | нет | `false` | Если `true`, значение является массивом элементов указанного типа |
| `required` | boolean | нет | `false` | Значение обязательно; несовместимо с `default` |
| `default` | scalar или array | нет | — | Значение по умолчанию; тип и форма должны соответствовать `type` и `array` |
| `hosts` | array | нет | все включённые | Для `type=host`: допустимые ID внешних хостов; для других типов запрещено |
| `min` | number | нет | — | Минимум для `integer`/`number`, включительно; для массива применяется к каждому элементу |
| `max` | number | нет | — | Максимум для `integer`/`number`, включительно; для массива применяется к каждому элементу |

Правила:

- в одном скрипте не более 64 параметров; `key` уникальны в пределах скрипта;
- `storage` по умолчанию равен `runtime`; другие значения не допускаются;
- порядок `parameters` задаёт порядок полей в клиенте: runtime-параметры — в форме запуска, параметры `storage=host` — в окне «Параметры Transmission»;
- `array=false` означает одно скалярное значение; `array=true` — массив от 0 до 256 элементов, а для `required=true` — от 1 до 256;
- вложенные массивы, объекты и `null` в значениях запрещены;
- `string`: валидный UTF-8 без NUL, не длиннее 4096 байт; пустая строка допустима, если параметр не `required`;
- `integer`: целое число в безопасном диапазоне JSON `-9007199254740991..9007199254740991`;
- `number`: конечное число JSON; `min` и `max` проверяются включительно;
- `boolean`: только `true` или `false`;
- `host`: значение является `id` из `external_hosts`; клиент не может передать URL, `netrc_machine` или иные данные хоста;
- `hosts` допускается только для `type=host`; если поле не задано, разрешены все включённые внешние хосты; если задано, оно непустое, а все ID существуют и включены;
- если у скрипта есть параметр `type=host`, эффективный список `hosts` должен содержать хотя бы один включённый хост;
- `default` для `type=host` должен входить в эффективный список `hosts`;
- `min` и `max` допускаются только для `integer`/`number`;
- `default` проходит те же проверки, что и значение параметра; для `array=true` он обязан быть массивом; `required=true` и `default` вместе запрещены;
- для `storage=runtime`: если необязательный параметр не передан и у него нет `default`, ключ отсутствует в `parameters` Execution Payload; если параметр передан, его значение заменяет `default`;
- для `storage=host`: сохранённое значение заменяет `default`; если значения нет и есть `default`, используется `default`; если нет ни того, ни другого и параметр необязательный, ключ отсутствует; если параметр обязательный, запуск возвращает `script settings incomplete`;
- клиент не может передать в `script_run` ключ, отсутствующий в описании или имеющий `storage=host`; такой запрос отклоняется;
- значения проверяет демон; семантическую проверку выполняет скрипт.

Пример описания всех типов:

```json
{
  "parameters": [
    {"key": "priority", "name": "Priority", "type": "integer", "storage": "runtime", "default": 0, "min": -1, "max": 1},
    {"key": "ratio", "name": "Ratio", "type": "number", "storage": "runtime", "min": 0, "max": 10},
    {"key": "dry_run", "name": "Dry run", "type": "boolean", "storage": "runtime", "default": false},
    {"key": "label", "name": "Label", "type": "string", "storage": "runtime", "required": true},
    {"key": "tags", "name": "Tags", "type": "string", "storage": "runtime", "array": true},
    {"key": "target", "name": "Target host", "type": "host", "storage": "runtime", "required": true, "hosts": ["qnap", "samsung-pc"]},
    {"key": "archive_dir", "name": "Archive directory", "type": "string", "storage": "host", "required": true},
    {"key": "keep_days", "name": "Keep days", "type": "integer", "storage": "host", "default": 30, "min": 1, "max": 365}
  ]
}
```

#### 3.5.1. Значения параметров `storage=host`

Значения параметров со `storage=host` хранятся в отдельном файле на хосте демона:

```text
<config-dir>/script-runner-values.json
```

Пример:

```json
{
  "schema": "transgui-script-runner-values/v1",
  "scripts": {
    "copy-to-host": {
      "copy_retries": 5
    },
    "archive-cleanup": {
      "archive_dir": "/srv/archive",
      "keep_days": 14
    }
  }
}
```

Правила:

- файл необязателен; при отсутствии используются только `default`, а обязательные параметры `storage=host` без значения дают `script settings incomplete`;
- файл читается при старте демона, при `SIGHUP`/reload на Linux и после успешного `script_settings_set`; на Windows запись через `script_settings_set` применяется сразу, перезапуск службы не требуется;
- файл содержит только параметры со `storage=host`; ключи `runtime`-параметров и ключи неизвестных скриптов/параметров игнорируются с записью в лог и могут быть удалены при следующей записи;
- значения проходят те же проверки типа, формы, `required`, `min`, `max` и `hosts`, что и runtime-значения; невалидное значение игнорируется с записью в лог, после чего применяется `default` или параметр считается отсутствующим;
- максимальный размер файла — 1 MiB; в нём не более 256 скриптов и не более 64 параметров на скрипт;
- файл и каталог доступны на запись только администратору, системе и пользователю демона; на POSIX файл не должен быть group-writable или world-writable; на Windows ACL не должен разрешать запись `Everyone`, `Users` или `Authenticated Users`;
- если файл отсутствует, это нормальный случай: хранилище пустое, `script_settings_get` возвращает значения по умолчанию;
- если файл не читается, содержит невалидный JSON/схему или имеет недопустимые права, хранилище значений `storage=host` считается недоступным, ошибка пишется в лог, runner продолжает работать для runtime-параметров, а `script_settings_get`/`script_settings_set` возвращают `script settings unavailable`;
- запись выполняется атомарно: новый файл пишется во временный файл в том же каталоге, проверяется и заменяет старый; при ошибке записи старые значения сохраняются;
- одновременные записи сериализуются; оптимистическая блокировка выполняется через `revision`;
- `revision` значений `storage=host` вычисляется как `sha256:` + SHA-256 от канонического UTF-8 JSON объекта `{"schema":"transgui-script-runner-values/v1","scripts":{...}}` с сортировкой ключей и без пробелов; `revision` не хранится в файле и возвращается RPC;
- изменение значений `storage=host` не меняет `revision` descriptor-а и не требует reload `script-runner.json`, но меняет `revision` значений `storage=host`;
- новые jobs используют значения на момент постановки в очередь; queued и running jobs сохраняют снимок значений, с которым были созданы;
- значения `storage=host` не являются секретами; пароли и токены в них хранить запрещено.

#### 3.5.2. Управление в окне «Параметры Transmission»

Клиент TransGUI показывает параметры `storage=host` в окне «Параметры Transmission» (`Transmission options`), на отдельной вкладке «Параметры скриптов» (`Script parameters`).

Требования к клиенту:

- вкладка видима только при наличии feature `script_runner_settings/v1` и хотя бы одного скрипта с параметром `storage=host`;
- вкладка показывает только параметры `storage=host`; runtime-параметры остаются в форме запуска скрипта;
- форма строится по descriptor: `type`, `array`, `required`, `default`, `min`, `max`, `hosts`; для `type=host` используется `name` из `external_hosts`;
- значения загружаются через `script_settings_get` при открытии вкладки и не хранятся в `transgui.ini`;
- при нажатии OK изменения отправляются через `script_settings_set` с текущим `revision`; при нажатии Cancel изменения не сохраняются;
- окно всегда относится к текущему подключению/хосту; при переключении подключения показываются значения соответствующего демона;
- если обязательный параметр `storage=host` не задан, скрипт остаётся в меню, но клиент показывает предупреждение и не позволяет запуск до заполнения настройки;
- ошибки `script settings unavailable`, `script settings incomplete` и `script settings revision conflict` показываются пользователю без закрытия окна;
- значения `storage=host` являются общими для всех клиентов этого хоста; локальные переопределения в `transgui.ini` не поддерживаются.

### 3.6. Внешние хосты

`external_hosts` — необязательный административный каталог внешних демонов, к которым могут обращаться оркестраторные скрипты. Клиент выбирает только `id` из каталога; URL и netrc machine клиенту не передаются.

| Поле | Тип | Обязательно | По умолчанию | Описание |
|---|---|---:|---:|---|
| `id` | string | да | — | Уникальный идентификатор: `[a-z][a-z0-9._-]{0,63}` |
| `name` | string | нет | `id` | Отображаемое имя, до 128 символов |
| `description` | string | нет | `""` | Описание для клиента, до 1024 символов |
| `enabled` | boolean | нет | `true` | Выключенный хост не анонсируется и не может быть выбран |
| `rpc_url` | string | да | — | Абсолютный URL RPC внешнего демона; схема `http` или `https`, без логина, пароля, query и fragment |
| `netrc_machine` | string | нет | `id` | Имя `machine` в `rpc_netrc_file`; `[A-Za-z0-9._:-]{1,255}` |

Правила:

- в каталоге не более 64 хостов; `id` уникальны;
- в descriptor и в допустимые значения параметров `type=host` попадают только включённые хосты;
- `rpc_url` задаёт администратор; клиент не может передать или изменить URL, `netrc_machine` или иные данные хоста;
- `rpc_url` может использовать `http` или `https`; TLS для внешнего хоста проверяет HTTP-клиент скрипта, а не runner; локальный RPC runner-а остаётся HTTP-only;
- `rpc_url` не содержит учётных данных; логин и пароль берутся из netrc;
- `netrc_machine` выбирает запись `machine` в общем `rpc_netrc_file`; если netrc не настроен, скрипт использует собственный защищённый источник кредов;
- в публичный descriptor входят только `id`, `name` и `description`; `rpc_url` и `netrc_machine` являются приватными;
- при reload каталог заменяется целиком; jobs используют снимок конфигурации, с которым были созданы.

### 3.7. Перезапуск демона (контекст system)

`restart_daemon` — необязательный объект настройки отдельной административной операции в контексте `system`; это не script job и не элемент `scripts`. Runner не повышает права и не запускает произвольные команды: он вызывает заранее настроенный helper, которому service manager или администратор дал необходимые права.

| Поле | Тип | Обязательно | По умолчанию | Описание |
|---|---|---:|---:|---|
| `enabled` | boolean | нет | `false` | Включает RPC-метод и feature `script_restart_daemon/v1` |
| `helper` | string | да при `enabled=true` | — | Абсолютный путь к helper-у; на Windows только `.exe`, на Linux — исполняемый файл |
| `args` | array | нет | `[]` | Фиксированные аргументы helper-а; shell не используется |
| `cwd` | string | да при `enabled=true` | — | Абсолютный существующий рабочий каталог helper-а |
| `timeout_seconds` | integer | нет | 30 | Таймаут запуска helper-а; 1..300 |
| `delay_seconds` | integer | нет | 2 | Задержка перед запуском helper-а после ответа; 0..300 |
| `cooldown_seconds` | integer | нет | 60 | Минимальный интервал между принятыми перезапусками; 0..86400 |

Правила:

- по умолчанию перезапуск выключен; при `enabled=false` feature `script_restart_daemon/v1` отсутствует, а метод возвращает `restart is disabled`;
- helper запускается без shell, с фиксированными `args` и `cwd`; клиент не может передать команду, аргументы или окружение;
- helper должен быть обычным исполняемым файлом, принадлежать администратору/системе и быть недоступным на запись пользователю демона; на Windows запрещены `.bat`, `.cmd` и `.ps1`;
- до ответа runner создаёт отсоединённый процесс-планировщик, который ждёт `delay_seconds` и запускает helper; клиент обязан корректно обрабатывать разрыв соединения;
- одновременно допускается только один запланированный перезапуск; повторный запрос до запуска helper-а возвращает `restart already scheduled`;
- после принятого перезапуска действует `cooldown_seconds`; запрос в период cooldown возвращает `restart cooldown active`;
- если не удалось создать планировщик или подготовить запуск, возвращается `restart helper failed`; ошибки запуска helper-а планировщиком после ответа только логируются;
- вывод helper-а ограничивается и не возвращается клиенту; секреты в `args` хранить нельзя;
- перезапуск завершает активные jobs вместе с демоном; после старта jobs не восстанавливаются, конфигурация читается заново;
- невалидная конфигурация `restart_daemon` при `enabled=true` выключает runner (fail closed);
- пример Linux helper-а: `systemctl --no-block restart transmission-daemon` через настроенный sudoers/polkit;
- на Windows `sc.exe` не имеет команды `restart`: одной команды `sc` недостаточно. Helper обязан быть отдельным `.exe`, который через SCM API либо последовательность `sc stop`, ожидание остановки и `sc start` перезапускает службу; `.bat`, `.cmd` и `.ps1` запрещены;
- Windows-планировщик и helper должны переживать остановку демона: запускаться как отдельная служба/задача или отсоединённым процессом вне Job Object демона, иначе они будут завершены вместе с демоном.

### 3.8. Окружение процесса

Демон не наследует всё своё окружение. Скрипт получает минимальный набор переменных ОС плюс `env` из конфигурации:

- Linux: `PATH`, `HOME`, `LANG`, `LC_ALL` (если задана), `TMPDIR` (если задана);
- Windows: `SystemRoot`, `SystemDrive`, `PATH`, `TEMP`, `TMP`, `USERPROFILE`.

Если `env` задаёт переменную с тем же именем, значение из `env` имеет приоритет. На Windows имена переменных регистронезависимы.

Имена переменных должны соответствовать `[A-Za-z_][A-Za-z0-9_]*`. Значения должны быть строками без NUL. Максимум 64 переменные, каждая не длиннее 4096 байт.

Дополнительно демон всегда задаёт `TR_SCRIPT_RPC_URL` с URL локального RPC и `TR_SCRIPT_RPC_NETRC` с путём к netrc, если эти значения доступны. Эти переменные не содержат секретов; `env` из конфигурации может их переопределить.

## 4. Валидация конфигурации

При загрузке runner обязан проверить:

- корректность JSON;
- соответствие `schema`;
- типы и диапазоны полей верхнего уровня;
- права на файл конфигурации и каталог скриптов; при нарушении runner выключается;
- уникальность `id`;
- непустой набор уникальных `contexts`; допустимы `torrent`, `file` и `system`; `system` должен быть единственным контекстом, а `requires_selection` для него обязан быть `false`;
- поддержку `payload` и `stdin`;
- допустимое значение `platform`; несовпадение с текущей ОС не делает скрипт невалидным, но исключает его из descriptor;
- абсолютность `executable` и `cwd`;
- существование и исполнимость `executable`;
- что `executable` после разрешения symlink является обычным файлом;
- что `executable` не является `.bat`, `.cmd` или `.ps1` на Windows;
- что `cwd` существует и является каталогом;
- отсутствие пустых строк и NUL в `args`;
- допустимые имена и значения `env`;
- корректность `parameters`: не более 64 параметров, уникальные `key` по `[a-z][a-z0-9_]{0,63}`, непустые `name` до 128 символов, `description` до 1024 символов, допустимые `type`, `storage`, `array`, `required`, `default`, `min` и `max`;
- что `storage` равен `runtime` или `host`; отсутствие `storage` означает `runtime`;
- что `required` и `default` не заданы одновременно;
- что `default` соответствует `type` и `array`, проходит проверки `min`/`max`, а для `integer` является целым;
- что `min` и `max` заданы только для `integer`/`number`, являются конечными числами и `min <= max`;
- что строковые значения параметров не длиннее 4096 байт, массивы не длиннее 256 элементов, а описания и имена не превышают лимиты;
- что `script-runner-values.json`, если он существует, читается, имеет схему `transgui-script-runner-values/v1`, размер не более 1 MiB и допустимые права; при нарушении хранилище значений `storage=host` считается недоступным, но runner продолжает работать;
- что в `script-runner-values.json` ключи соответствуют известным параметрам `storage=host`, а значения проходят проверки `type`, `array`, `required`, `min`, `max` и `hosts`; невалидные записи игнорируются с записью в лог;
- корректность `external_hosts`: не более 64 хостов, уникальные `id`, допустимые `name`, `description`, `enabled`, `rpc_url` и `netrc_machine`;
- что `rpc_url` внешнего хоста является абсолютным `http`/`https` URL без логина, пароля, query и fragment;
- что все ID в поле `hosts` уникальны, существуют, включены и входят в эффективный список, а у параметра `type=host` есть хотя бы один доступный хост; `default` также входит в этот список;
- что `hosts` задан только для `type=host`, а `min`/`max` — только для `integer`/`number`;
- при `restart_daemon.enabled=true`: абсолютность и исполнимость `helper`, отсутствие пустых строк и NUL в `args`, существование `cwd`, допустимые `timeout_seconds`, `delay_seconds` и `cooldown_seconds`;
- что helper принадлежит администратору/системе и не доступен на запись пользователю демона; на Windows helper имеет расширение `.exe`, а `.bat`, `.cmd` и `.ps1` запрещены;
- допустимые значения `timeout_seconds` и `max_output_bytes`;
- отсутствие world-writable `executable`, `cwd` и файлов, на которые указывают `args`, на POSIX;
- что `max_total_output_bytes` не меньше удвоенного максимального эффективного `max_output_bytes` среди скриптов, доступных на текущей платформе;
- что `queue_limit_per_client` не больше `queue_limit`, а `max_concurrent_per_client` не больше `max_concurrent`;
- что `rpc_netrc_file`, если задан, является абсолютным путём к обычному файлу, доступному на чтение только администратору, системе и пользователю демона; при нарушении runner выключается;
- количество скриптов и длину строковых полей.

Некорректный скрипт не регистрируется. Ошибка записывается в лог. Остальные корректные скрипты продолжают работать.

Некорректный внешний хост не регистрируется; параметры `type=host`, ссылающиеся на него, делают соответствующий скрипт невалидным. Остальные хосты и скрипты продолжают работать.

Недоступное хранилище значений `storage=host` не выключает runner. Скрипты с параметрами `storage=host` остаются анонсированы, но `script_run` для них возвращает `script settings unavailable` (это приоритетнее, чем `script settings incomplete`); runtime-скрипты продолжают работать. Ошибка хранилища пишется в лог.

Если `restart_daemon.enabled=true` и его конфигурация невалидна, runner выключается (fail closed), потому что операция связана с повышенными правами.

Если некорректен весь файл конфигурации, неверны его права или права каталога скриптов, runner считается выключенным, ошибка записывается в лог.

Скрипты с `platform`, не соответствующей текущей ОС, остаются известны runner-у для диагностики, но не попадают в descriptor. `script_run` для них возвращает `script is not available on this platform`.

При reload действуют те же правила. Валидная новая конфигурация заменяет старую; невалидная выключает runner по правилам раздела 3.1. Значения параметров `storage=host` перечитываются независимо от `script-runner.json`; ошибка значений не выключает runner и не отменяет уже запущенные jobs.

## 5. Анонсирование

### 5.1. Feature flag

Если runner включён, конфигурация валидна, текущая ОС поддерживается и права в порядке, демон добавляет в `fork-features`:

```text
script_runner/v1
script_runner_settings/v1
```

`script_runner_settings/v1` означает наличие RPC-методов `script_settings_get` и `script_settings_set`, даже если в конфигурации пока нет ни одного параметра `storage=host` или файл значений пуст. Если хранилище значений `storage=host` недоступно, feature остаётся, а методы возвращают `script settings unavailable`.

Если `restart_daemon.enabled=true` и helper валиден, дополнительно добавляется:

```text
script_restart_daemon/v1
```

Если runner выключен, конфигурация не загружена, невалидна или ОС не поддерживается, все feature отсутствуют. Если перезапуск выключен, отсутствует только `script_restart_daemon/v1`; если хранилище значений недоступно, отсутствует только работоспособность методов настроек, но не сама feature.

Наличие `script_runner/v1` не зависит от того, есть ли в конфигурации хотя бы один скрипт.

### 5.2. Поле в session-get

При `session-get` с полем `script_runner` демон возвращает:

```json
{
  "schema": "transgui-script-runner/v1",
  "revision": "sha256:...",
  "external_hosts": [
    {"id": "qnap", "name": "QNAP", "description": "QNAP Transmission"},
    {"id": "samsung-pc", "name": "samsung-pc", "description": "Windows Transmission"}
  ],
  "restart_daemon": {
    "id": "restart-daemon",
    "name": "Restart daemon",
    "context": "system",
    "enabled": false
  },
  "scripts": [
    {
      "id": "torrent-priority",
      "name": "Set file priorities",
      "description": "Set file priorities for selected torrents",
      "group": "Torrents",
      "order": 10,
      "platform": "any",
      "contexts": ["torrent"],
      "requires_selection": true,
      "confirm": true,
      "parameters": [
        {"key": "priority", "name": "Priority", "description": "", "type": "integer", "storage": "runtime", "array": false, "required": false, "default": 0, "min": -1, "max": 1},
        {"key": "dry_run", "name": "Dry run", "description": "", "type": "boolean", "storage": "runtime", "array": false, "required": false, "default": false}
      ],
      "payload": "transgui.script/v1",
      "stdin": "json",
      "timeout_seconds": 300,
      "max_output_bytes": 1048576
    },
    {
      "id": "archive-cleanup",
      "name": "Cleanup archive",
      "description": "Delete old files from the archive",
      "group": "System",
      "order": 40,
      "platform": "any",
      "contexts": ["system"],
      "requires_selection": false,
      "confirm": true,
      "parameters": [
        {"key": "archive_dir", "name": "Archive directory", "description": "", "type": "string", "storage": "host", "array": false, "required": true},
        {"key": "keep_days", "name": "Keep days", "description": "", "type": "integer", "storage": "host", "array": false, "required": false, "default": 30, "min": 1, "max": 365},
        {"key": "dry_run", "name": "Dry run", "description": "", "type": "boolean", "storage": "runtime", "array": false, "required": false, "default": false}
      ],
      "payload": "transgui.script/v1",
      "stdin": "json",
      "timeout_seconds": 3600,
      "max_output_bytes": 1048576
    }
  ]
}
```

Если runner выключен, поле `script_runner` отсутствует в ответе `session-get`. Клиент обязан проверять feature перед использованием поля.

В descriptor попадают только включённые скрипты, соответствующие текущей платформе. `description` и `group` передаются как пустые строки, если не заданы. `max_output_bytes` содержит эффективное значение с учётом `max_output_bytes_default`.

`parameters` содержит публичные описания параметров и присутствует всегда; если параметров нет, передаётся `[]`. Поля `storage`, `array` и `required` передаются всегда, `default`, `min` и `max` — только если заданы в конфигурации; для `type=host` поле `hosts` передаётся как эффективный список разрешённых ID. `description` передаётся пустой строкой, если не задана. Клиент строит форму запуска по параметрам `storage=runtime`, а форму host-настроек — по параметрам `storage=host`; runtime-значения передаются в `payload.parameters`, значения `storage=host` — через `script_settings_set`. Значения параметров в descriptor не входят.

`external_hosts` содержит публичные данные только включённых хостов, отсортированных по `id`: `id`, `name` и `description`. `restart_daemon` содержит фиксированные `id` (`restart-daemon`), `name`, `context` (`system`) и `enabled`. Действие перезапуска относится к контексту `system`, а не к скриптам. Клиент сопоставляет ID из параметров `type=host` с `external_hosts` и показывает `name`.

В descriptor не передаются:

- `executable`;
- `args`;
- `cwd`;
- `env`;
- `rpc.url`;
- `rpc.netrc`;
- `external_hosts[].rpc_url`;
- `external_hosts[].netrc_machine`;
- `restart_daemon.helper`;
- `restart_daemon.args`;
- `restart_daemon.cwd`;
- `job_token`;
- значения runtime-параметров и параметров `storage=host`;
- любые секреты и внутренние пути.

`revision` вычисляется при загрузке конфигурации как `sha256:` + SHA-256 от канонического UTF-8 JSON объекта:

```json
{
  "schema": "transgui-script-runner/v1",
  "external_hosts": [],
  "restart_daemon": {
    "enabled": false
  },
  "scripts": []
}
```

В `scripts` входят публичные descriptor-поля всех анонсируемых скриптов, включая `parameters` с их `storage`, отсортированных по `id`; `external_hosts` содержит публичные поля включённых хостов, отсортированных по `id`; ключи объектов сериализуются в стабильном порядке, без пробелов. `revision` меняется при изменении списка скриптов, описаний параметров или `storage`, публичного каталога хостов или `restart_daemon.enabled`. Приватные поля, включая `executable`, `args`, `cwd`, `env`, `external_hosts[].rpc_url`, `restart_daemon.helper` и значения параметров `storage=host`, в хеш не входят. `revision` не зависит от форматирования файла конфигурации и от `script-runner-values.json`.

Клиент строит меню по `contexts`, `group`, `order` и `name`, форму запуска — по runtime-параметрам, а форму host-настроек — по параметрам `storage=host`. Набор скриптов и значения `storage=host` зависят от конкретного хоста.

## 6. RPC API

### 6.1. script_run

Запрос:

```json
{
  "method": "script_run",
  "arguments": {
    "id": "torrent-priority",
    "client_id": "transgui-sergeybo02",
    "client_request_id": "7b3c2f0e-4a1d-4c9e-9f4a-2d7a7d9b1c11",
    "client_secret": "c7d1e4a9b2f0836a5e4d3c2b1a0918273645f6e7d8c9b0a1",
    "payload": {
      "schema": "transgui.script/v1",
      "context": "torrent",
      "host": "sergeybo02",
      "parameters": {
        "priority": 1,
        "dry_run": false
      },
      "selection": {
        "hashes": [
          "0123456789abcdef0123456789abcdef01234567",
          "89abcdef0123456789abcdef0123456789abcdef"
        ],
        "files": []
      }
    }
  }
}
```

Ответ при успешной постановке в очередь:

```json
{
  "job_id": "5c9f7c1e-...",
  "job_token": "9f4a2d7a7d9b1c11...",
  "script_id": "torrent-priority",
  "state": "queued",
  "deduplicated": false
}
```

`job_token` — секрет, который генерируется демоном из 32 случайных байт и возвращается в hex-виде (64 символа `[0-9a-f]`). Он требуется для `script_status` и `script_cancel`, не передаётся скрипту и не логируется. Сравнение токена выполняется в постоянное время. Токен действует, пока job хранится; после eviction или перезапуска демона он недействителен. Потерянный `job_token` можно восстановить повторным `script_run` с той же парой `(client_request_id, client_secret)`; оба значения должны быть непредсказуемыми и не должны логироваться.

`client_id` необязателен. Допустимая длина — 1..128 символов, допустимые символы — `[A-Za-z0-9._:-]`. Он используется только для `queue_limit_per_client` и `max_concurrent_per_client`; если не задан, применяется общий bucket `default`. `client_id` не является средством авторизации.

`client_request_id` и `client_secret` необязательны. `client_request_id` — 32..128 символов `[A-Za-z0-9._:-]`; `client_secret` — 32..128 символов `[A-Za-z0-9_-]`. `client_secret` обязателен, если задан `client_request_id`, и запрещён без него. Оба значения должны быть случайными, с энтропией не менее 128 бит. Демон хранит только SHA-256 от обоих значений и сравнивает их в постоянное время. Пространство ключей общее для всех RPC-клиентов и ограничено тройкой `(script_id, client_request_id, client_secret)`. Если для той же тройки уже существует активный или сохранённый job с тем же Request Payload, демон возвращает существующий job с текущим состоянием и `deduplicated: true`. Сравнение Request Payload выполняется по каноническому JSON в том виде, в котором его передал клиент; `default` в Request Payload не подставляются, поэтому опущенный необязательный параметр и явно переданное значение по умолчанию считаются разными payload. Если ключ или секрет не совпадают либо payload отличается, возвращается ошибка `script request id conflict`. После удаления job по retention или по лимитам ключ может быть использован повторно.

Демон обязан проверить:

- существование и включённость `id`;
- доступность скрипта на текущей платформе;
- допустимость контекста: `torrent`, `file` или `system`; для `system` выбор должен отсутствовать или быть пустым;
- соответствие Request Payload версии и схеме;
- соответствие `payload.parameters` описанию скрипта: только объявленные ключи со `storage=runtime`, правильные типы, форма массива, `required`, `default`, `min` и `max`; ключ `storage=host` или неизвестный ключ дают `invalid script parameters`;
- для `type=host`: runtime-значения являются `id` включённых хостов и входят в эффективный список `hosts`; URL и `netrc_machine` от клиента не принимаются;
- доступность хранилища значений `storage=host`, если у скрипта есть параметры `storage=host`; если хранилище недоступно, возвращается `script settings unavailable`, и это приоритетнее проверки значений;
- наличие валидных значений для обязательных параметров `storage=host`; если хранилище доступно, но значения и `default` отсутствуют, возвращается `script settings incomplete`;
- размер payload не больше `max_payload_bytes`, иначе `script payload is too large`;
- количество элементов выбора не больше `max_selection_items`, иначе `script selection is too large`;
- существование всех хешей и корректность всех `file_index`;
- принадлежность каждого файла указанному торренту;
- наличие выбранных элементов, если `requires_selection = true`;
- формат `client_id`, если он задан; при нарушении `invalid script payload`;
- корректность пары `client_request_id`/`client_secret`: `client_secret` обязателен при наличии `client_request_id` и запрещён без него; иначе `invalid script payload`;
- отсутствие конфликта `client_request_id` и `client_secret`; эта проверка выполняется до проверки очереди;
- отсутствие переполнения общей очереди и очереди клиента `client_id`;
- отсутствие превышения `max_concurrent_per_client` для `client_id`.

Клиент не может передать `executable`, `args`, `cwd`, `env`, `path`, `download_dir`, `rpc`, внешние `rpc_url`/`netrc_machine` или иные серверные данные. Все они формируются демоном из конфигурации. В `payload.parameters` разрешены только ключи с `storage=runtime`, объявленные в descriptor скрипта; неизвестные ключи и ключи `storage=host` отклоняются. Значения `storage=host` демон берёт из `script-runner-values.json`.

Выполнение асинхронное. HTTP-запрос не должен ждать завершения скрипта. Начальное состояние нового job в ответе всегда `queued`; фактически job может уже выполняться.

### 6.2. script_status

Запрос:

```json
{
  "method": "script_status",
  "arguments": {
    "job_id": "5c9f7c1e-...",
    "job_token": "9f4a2d7a7d9b1c11...",
    "stdout_offset": 0,
    "stdout_limit": 65536,
    "stderr_offset": 0,
    "stderr_limit": 65536
  }
}
```

`job_token` обязателен. Если job не существует или токен не совпадает, возвращается `job not found`.

`stdout_offset` и `stderr_offset` по умолчанию 0. `stdout_limit` и `stderr_limit` по умолчанию 65536, максимум 1048576. Отрицательные значения и превышение максимума дают ошибку `invalid output range`. Если offset больше `*_stored_bytes`, возвращается пустая строка.

Ответ:

```json
{
  "job_id": "5c9f7c1e-...",
  "script_id": "torrent-priority",
  "state": "finished",
  "exit_code": 0,
  "term_signal": null,
  "error": null,
  "stdout": "Connected to ...",
  "stderr": "",
  "stdout_offset": 0,
  "stdout_total_bytes": 16,
  "stdout_stored_bytes": 16,
  "stdout_truncated": false,
  "stderr_offset": 0,
  "stderr_total_bytes": 0,
  "stderr_stored_bytes": 0,
  "stderr_truncated": false,
  "truncated": false,
  "timed_out": false,
  "queued_at": 1761579990,
  "started_at": 1761580000,
  "finished_at": 1761580012
}
```

Состояния job:

- `queued`;
- `running`;
- `finished`;
- `failed`;
- `canceled`;
- `timed_out`.

Правила состояний и полей:

- `finished` — процесс завершился нормально с кодом 0;
- `failed` — ненулевой код возврата, завершение по сигналу (кроме отмены и таймаута), ошибка запуска или внутренняя ошибка;
- `timed_out` — истёк `timeout_seconds`; процесс и потомки принудительно завершены;
- `canceled` — job отменён до завершения;
- `exit_code` — целое число для нормального завершения; `null` для queued/running, завершения по сигналу, таймаута и отмены;
- `term_signal` — номер сигнала POSIX, если процесс завершился по сигналу; иначе `null`;
- `error` — текст ошибки запуска или внутренней ошибки; иначе `null`;
- `timed_out` — `true` только для состояния `timed_out`;
- `queued_at`, `started_at`, `finished_at` — epoch seconds; `null`, если событие ещё не произошло;
- `stdout_total_bytes` и `stderr_total_bytes` — сколько байт нормализованного UTF-8 вывода реально произвёл процесс;
- `stdout_stored_bytes` и `stderr_stored_bytes` — сколько байт сохранено с учётом `max_output_bytes`;
- `stdout_truncated` и `stderr_truncated` — `true`, если сохранено меньше, чем произведено;
- `truncated` — `stdout_truncated || stderr_truncated`;
- `stdout` и `stderr` — UTF-8 срезы сохранённого вывода от соответствующего offset, не длиннее limit; offset и limit задаются в байтах нормализованного UTF-8, демон корректирует границы, чтобы не разрывать многобайтовую последовательность.

Для queued job вывод пустой, `started_at` и `finished_at` равны `null`. Для running job возвращается уже накопленный вывод и текущие счётчики, `finished_at` равен `null`. Для завершённых jobs вывод остаётся доступным до retention или eviction.

Поля `stdout` и `stderr` возвращаются как UTF-8. Некорректные UTF-8 последовательности и NUL заменяются на U+FFFD. При превышении лимита вывод продолжает читаться и отбрасываться, процесс не блокируется, а соответствующий `*_truncated` устанавливается в `true`.

### 6.3. script_cancel

Запрос:

```json
{
  "method": "script_cancel",
  "arguments": {
    "job_id": "5c9f7c1e-...",
    "job_token": "9f4a2d7a7d9b1c11..."
  }
}
```

Ответ:

```json
{
  "job_id": "5c9f7c1e-...",
  "canceled": true,
  "state": "canceled"
}
```

Правила:

- queued job удаляется из очереди и сразу переходит в `canceled`;
- running job получает запрос на завершение; ответ возвращается сразу, `canceled: true`, а `state` в ответе отражает текущее состояние (`running` или уже `canceled`);
- завершение процесса и всех потомков выполняется принудительно: на Linux сначала SIGTERM, через 5 секунд SIGKILL; на Windows — `TerminateJobObject`;
- для уже завершённого job возвращается `canceled: false` и текущее состояние;
- если job завершился одновременно с отменой, побеждает фактическое завершение, `canceled` равен `false`;
- `job_token` обязателен; при отсутствии, неверном токене или неизвестном job возвращается `job not found`;
- отменять и читать job может только клиент, предъявивший правильный `job_token`.

### 6.4. script_restart_daemon

Отдельный административный метод в контексте `system`; он не создаёт job и не использует `job_token`.

Запрос:

```json
{
  "method": "script_restart_daemon",
  "arguments": {
    "client_id": "transgui-sergeybo02"
  }
}
```

Ответ при принятом перезапуске:

```json
{
  "accepted": true,
  "restart_id": "b7d9c9a2-...",
  "state": "scheduled",
  "scheduled_at": 1761580002
}
```

Правила:

- метод доступен только при feature `script_restart_daemon/v1`; иначе возвращается `restart is disabled`;
- `client_id` необязателен и используется только для аудита; формат тот же, что в `script_run`;
- одновременно допускается только один запланированный перезапуск; повторный запрос до запуска helper-а возвращает `restart already scheduled`;
- после принятого перезапуска действует `cooldown_seconds`; запрос в период cooldown возвращает `restart cooldown active`;
- демон фиксирует `restart_id`, `scheduled_at` и создаёт отсоединённый планировщик до ответа; планировщик запускает helper после `delay_seconds`;
- клиент должен корректно обрабатывать разрыв соединения: helper может остановить демон сразу после ответа;
- если не удалось создать планировщик или подготовить запуск, возвращается `restart helper failed`; ошибки запуска helper-а после ответа только логируются;
- перезапуск завершает активные jobs вместе с демоном; после старта jobs не восстанавливаются, конфигурация читается заново;
- `restart_id` — случайный UUID для аудита, не секрет и не средство авторизации.

### 6.5. script_settings_get

Метод доступен при feature `script_runner_settings/v1`. Он возвращает сохранённые значения `storage=host` для всех включённых скриптов, доступных на текущей платформе, у которых есть параметры `storage=host`.

Запрос:

```json
{
  "method": "script_settings_get",
  "arguments": {}
}
```

Ответ:

```json
{
  "schema": "transgui-script-runner-values/v1",
  "revision": "sha256:...",
  "scripts": [
    {
      "id": "archive-cleanup",
      "parameters": {
        "archive_dir": "/srv/archive",
        "keep_days": 14
      },
      "ready": true,
      "missing": []
    },
    {
      "id": "copy-to-host",
      "parameters": {
        "copy_retries": 3
      },
      "ready": true,
      "missing": []
    }
  ]
}
```

Правила:

- в `scripts` входят только включённые скрипты текущей платформы с хотя бы одним параметром `storage=host`, отсортированные по `id`;
- `parameters` содержит эффективные значения параметров `storage=host`: сохранённое значение, иначе `default`; runtime-параметры и их значения не возвращаются;
- если обязательный параметр `storage=host` не имеет ни сохранённого значения, ни `default`, его ключ отсутствует в `parameters`, `ready` равен `false`, а ключ перечислен в `missing`;
- `revision` — ревизия значений `storage=host`, а не `revision` descriptor-а;
- при недоступном хранилище возвращается `script settings unavailable`;
- если runner выключен, возвращается `script runner is disabled`; если feature `script_runner_settings/v1` отсутствует, возвращается `script settings unavailable`;
- метод не требует `job_token` и доступен всем аутентифицированным RPC-клиентам.

### 6.6. script_settings_set

Метод доступен при feature `script_runner_settings/v1`. Он изменяет значения `storage=host` и применяет их к новым jobs без перезапуска демона.

Запрос:

```json
{
  "method": "script_settings_set",
  "arguments": {
    "expected_revision": "sha256:...",
    "scripts": [
      {
        "id": "archive-cleanup",
        "parameters": {
          "archive_dir": "/srv/archive2",
          "keep_days": 14
        }
      }
    ]
  }
}
```

Ответ:

```json
{
  "schema": "transgui-script-runner-values/v1",
  "revision": "sha256:...",
  "scripts": [
    {
      "id": "archive-cleanup",
      "parameters": {
        "archive_dir": "/srv/archive2",
        "keep_days": 14
      },
      "ready": true,
      "missing": []
    }
  ]
}
```

Правила:

- `scripts` содержит только скрипты, значения которых нужно изменить; скрипты, отсутствующие в массиве, не меняются;
- `parameters` для каждого скрипта является полной заменой сохранённых значений `storage=host` этого скрипта: отсутствующий необязательный ключ удаляется, после чего применяется `default`;
- разрешены только `id` существующих включённых скриптов, доступных на текущей платформе, и только ключи с `storage=host`; runtime-ключи, неизвестные ключи и скрипты без параметров `storage=host` дают `invalid script parameters`;
- значения проверяются по `type`, `array`, `required`, `min`, `max` и `hosts`; отсутствующий обязательный параметр `storage=host` после применения запроса даёт `script settings incomplete`;
- `expected_revision` необязателен; если он задан и не совпадает с текущей ревизией значений `storage=host`, возвращается `script settings revision conflict` и ничего не изменяется;
- запрос атомарен: либо применяются изменения всех перечисленных скриптов, либо ни одного;
- при ошибке записи возвращается `script settings unavailable`, старый файл сохраняется;
- при недоступном хранилище возвращается `script settings unavailable`;
- если runner выключен, возвращается `script runner is disabled`; если feature `script_runner_settings/v1` отсутствует, возвращается `script settings unavailable`;
- метод не требует `job_token` и доступен всем аутентифицированным RPC-клиентам; отдельной административной роли в v1 нет;
- изменения не меняют `revision` descriptor-а и не требуют reload `script-runner.json`;
- новые jobs используют новые значения; queued и running jobs продолжают использовать снимок, взятый при постановке в очередь.

## 7. Payload v1

### 7.1. Две формы payload

В v1 используются две формы с одинаковым `schema: transgui.script/v1`:

- **Request Payload** — то, что клиент передаёт в `script_run`; содержит селекторы и значения только runtime-параметров, объявленных для скрипта;
- **Execution Payload** — то, что демон передаёт скрипту в stdin; содержит разрешённые данные, значения runtime-параметров и параметров `storage=host` и серверные поля.

Клиент не может подменить Execution Payload. Поля `server`, `rpc`, `script`, `torrents`, `torrent_id`, `name`, `hash`, `download_dir`, `files`, `external_hosts`, `rpc_url` и учётные данные в Request Payload не используются и игнорируются.

Execution Payload содержит `rpc.url`, `rpc.netrc` и выбранные `external_hosts`; RPC-логины, пароли, cookie и иные секреты в payload не передаются.

### 7.2. Request Payload

Общая схема для torrent-контекста:

```json
{
  "schema": "transgui.script/v1",
  "context": "torrent",
  "host": "sergeybo02",
  "parameters": {
    "priority": 1,
    "dry_run": false
  },
  "selection": {
    "hashes": [
      "0123456789abcdef0123456789abcdef01234567",
      "89abcdef0123456789abcdef0123456789abcdef"
    ],
    "files": []
  }
}
```

Общая схема для file-контекста:

```json
{
  "schema": "transgui.script/v1",
  "context": "file",
  "host": "sergeybo02",
  "parameters": {
    "delete_data": true,
    "keep_extensions": [".nfo", ".srt"]
  },
  "selection": {
    "hashes": [],
    "files": [
      {
        "hash": "0123456789abcdef0123456789abcdef01234567",
        "file_index": 3
      }
    ]
  }
}
```

Общая схема для system-контекста:

```json
{
  "schema": "transgui.script/v1",
  "context": "system",
  "host": "sergeybo02",
  "parameters": {},
  "selection": {
    "hashes": [],
    "files": []
  }
}
```

Правила:

- `schema` обязателен и равен `transgui.script/v1`;
- `context` обязателен и равен `torrent`, `file` или `system`;
- `host` необязателен, до 255 символов; это метка подключения клиента, а не доверенные данные; если поле не задано, в Execution Payload передаётся пустая строка;
- `parameters` необязателен и должен быть объектом; если у скрипта нет runtime-параметров, поле можно опустить или передать `{}`;
- в `parameters` разрешены только ключи со `storage=runtime`; неизвестный ключ, ключ `storage=host`, `null`, объект или вложенный массив дают `invalid script parameters`;
- значения проверяются по `type`, `array`, `required`, `min`, `max` и `hosts`; строки не длиннее 4096 байт, массивы не длиннее 256 элементов;
- для `type=host` клиент передаёт только `id` включённого хоста из эффективного списка `hosts`; URL, `netrc_machine` и другие данные хоста клиент передать не может;
- отсутствующий необязательный runtime-параметр с `default` подставляется демоном; без `default` ключ отсутствует в Execution Payload; отсутствующий обязательный runtime-параметр даёт `invalid script parameters`;
- параметры `storage=host` в Request Payload отсутствуют; их значения демон берёт из `script-runner-values.json` и добавляет в Execution Payload;
- параметры не являются секретами и не должны использоваться для паролей и токенов;
- для `context=torrent` используется `selection.hashes`, а `selection.files` должен быть пустым;
- для `context=file` используется `selection.files`, а `selection.hashes` должен быть пустым;
- для `context=system` `selection` должен отсутствовать или содержать пустые `hashes` и `files`; скрипт не получает торренты и файлы;
- `hash` — 40 символов нижнего регистра `[0-9a-f]`;
- `file_index` — неотрицательное целое;
- при `requires_selection=true` выбор должен быть непустым;
- при `requires_selection=false` `selection` может отсутствовать или содержать пустые массивы;
- дубликаты хешей и пар `(hash, file_index)` удаляются с сохранением первого вхождения;
- общее число элементов `hashes + files` не должно превышать `max_selection_items`;
- размер Request Payload не должен превышать `max_payload_bytes`;
- сформированный Execution Payload также не должен превышать `max_payload_bytes`; при превышении возвращается `script payload is too large`.

Неизвестные поля верхнего уровня в Request Payload игнорируются; исключение — ключи внутри `parameters`, которые должны быть объявлены в descriptor и иметь `storage=runtime`. Серверные поля, перечисленные в 7.1, также игнорируются и не влияют на Execution Payload. Значения `storage=host` в Request Payload не принимаются и не могут переопределить сохранённые настройки хоста.

### 7.3. Execution Payload: общая схема

```json
{
  "schema": "transgui.script/v1",
  "context": "torrent",
  "host": "sergeybo02",
  "parameters": {},
  "external_hosts": {},
  "server": {
    "host": "sergeybo02",
    "os": "linux",
    "daemon_version": "4.2.0-dev (85278c4d6e)"
  },
  "rpc": {
    "url": "http://127.0.0.1:9091/transmission/rpc",
    "netrc": "/etc/transmission/script-rpc.netrc"
  },
  "script": {
    "id": "torrent-priority",
    "name": "Set file priorities"
  },
  "torrents": [],
  "torrent_id": [],
  "name": [],
  "hash": [],
  "download_dir": [],
  "files": []
}
```

`server` формирует демон. `server.host` — имя хоста демона, `server.os` — `linux` или `windows`, `server.daemon_version` — версия демона. Поле `server` следует использовать для идентификации сервера и ОС.

`script` формирует демон и всегда содержит `id` и `name` запускаемого скрипта.

`host` копируется из Request Payload и является недоверенной меткой клиента.

`parameters` формирует демон: в него входят значения runtime-параметров из Request Payload и значения параметров `storage=host` из `script-runner-values.json`. `default` уже применены, а необязательные параметры без `default` и без значения отсутствуют. Значения `storage=host` берутся из снимка, сделанного при постановке job в очередь. Объект `parameters` присутствует всегда и может быть пустым. Значения параметров проверены демоном; скрипт должен проверять их семантику.

`external_hosts` формирует демон из выбранных в параметрах `type=host` ID: в объект входят только включённые хосты, которые разрешены эффективным списком `hosts`. Для каждого хоста передаются `name`, `rpc_url` и `netrc_machine`. Объект присутствует всегда и может быть пустым. Клиент не может передать URL или netrc machine; он выбирает только ID.

Для `context=system` поля `torrents`, `torrent_id`, `name`, `hash`, `download_dir` и `files` всегда пустые; параметры и внешние хосты доступны так же, как в других контекстах.

`torrents` является основным источником данных. Плоские массивы `torrent_id`, `name`, `hash`, `download_dir` являются производными, имеют ту же длину и порядок, что и `torrents`.

`rpc.url` — URL локального RPC без учётных данных. `rpc.netrc` — путь к защищённому netrc-файлу или `null`, если автоматический netrc не настроен. Пароли и cookie в payload не передаются.

### 7.4. Torrent context

```json
{
  "schema": "transgui.script/v1",
  "context": "torrent",
  "host": "sergeybo02",
  "parameters": {
    "priority": 1,
    "dry_run": false
  },
  "external_hosts": {},
  "server": {
    "host": "sergeybo02",
    "os": "linux",
    "daemon_version": "4.2.0-dev (85278c4d6e)"
  },
  "rpc": {
    "url": "http://127.0.0.1:9091/transmission/rpc",
    "netrc": "/etc/transmission/script-rpc.netrc"
  },
  "script": {
    "id": "torrent-priority",
    "name": "Set file priorities"
  },
  "torrents": [
    {
      "torrent_id": 12,
      "name": "Movie A",
      "hash": "0123456789abcdef0123456789abcdef01234567",
      "download_dir": "/data"
    },
    {
      "torrent_id": 15,
      "name": "Movie B",
      "hash": "89abcdef0123456789abcdef0123456789abcdef",
      "download_dir": "/data"
    }
  ],
  "torrent_id": [12, 15],
  "name": ["Movie A", "Movie B"],
  "hash": [
    "0123456789abcdef0123456789abcdef01234567",
    "89abcdef0123456789abcdef0123456789abcdef"
  ],
  "download_dir": ["/data", "/data"],
  "files": []
}
```

Если `requires_selection = false`, массивы `torrents`, `torrent_id`, `name`, `hash`, `download_dir` и `files` могут быть пустыми.

Для `context=torrent` поле `files` всегда пустое. Полный список файлов доступен в file-контексте или через RPC.

`torrents` сохраняет порядок Request Payload после удаления дубликатов. Для file-контекста порядок `torrents` определяется первым появлением торрента в `files`.

`torrent_id` эфемерен и меняется после перезапуска демона. Скриптам следует использовать `hash` как стабильный идентификатор.

### 7.5. File context

```json
{
  "schema": "transgui.script/v1",
  "context": "file",
  "host": "sergeybo02",
  "parameters": {
    "delete_data": true,
    "keep_extensions": [".nfo", ".srt"]
  },
  "external_hosts": {},
  "server": {
    "host": "sergeybo02",
    "os": "linux",
    "daemon_version": "4.2.0-dev (85278c4d6e)"
  },
  "rpc": {
    "url": "http://127.0.0.1:9091/transmission/rpc",
    "netrc": "/etc/transmission/script-rpc.netrc"
  },
  "script": {
    "id": "file-delete",
    "name": "Delete selected files"
  },
  "torrents": [
    {
      "torrent_id": 12,
      "name": "Movie A",
      "hash": "0123456789abcdef0123456789abcdef01234567",
      "download_dir": "/data"
    }
  ],
  "torrent_id": [12],
  "name": ["Movie A"],
  "hash": ["0123456789abcdef0123456789abcdef01234567"],
  "download_dir": ["/data"],
  "files": [
    {
      "torrent_id": 12,
      "hash": "0123456789abcdef0123456789abcdef01234567",
      "file_index": 3,
      "relative_path": "Movie A/file.mkv",
      "path": "/data/Movie A/file.mkv",
      "name": "file.mkv",
      "size": 123456789,
      "wanted": true,
      "priority": 0,
      "selected": true
    },
    {
      "torrent_id": 12,
      "hash": "0123456789abcdef0123456789abcdef01234567",
      "file_index": 4,
      "relative_path": "Movie A/sample.nfo",
      "path": "/data/Movie A/sample.nfo",
      "name": "sample.nfo",
      "size": 1234,
      "wanted": false,
      "priority": -1,
      "selected": false
    }
  ]
}
```

Правила:

- `files` содержит все реальные файлы всех торрентов из `torrents`, а не только выбранные; папки не включаются;
- `selected` — `true`, если файл был выбран в Request Payload; это не то же самое, что `wanted`;
- `wanted` — текущий флаг wanted файла;
- `priority` — текущий приоритет файла: -1, 0 или 1;
- `relative_path` — путь относительно `download_dir`, всегда с разделителем `/`; для многофайлового торрента включает имя торрента;
- `path` — абсолютный путь в нативном формате ОС к текущему расположению файла; при включённом `incomplete-dir` путь может находиться вне `download_dir`; файл может ещё не существовать на диске, если он не скачан;
- `name` — имя файла без каталогов;
- `file_index` — индекс файла из RPC;
- `size` — размер файла в байтах;
- `download_dir` — база скачивания торрента из RPC (`downloadDir`), а не корень торрента;
- `files` упорядочен по порядку `torrents`, внутри торрента — по `file_index` по возрастанию;
- при выборе папки клиент разворачивает её в список файлов Request Payload; демон включает в `files` все файлы владельцев, а `selected` отмечает выбранные;
- для файлового контекста допускается несколько владельцев в `torrents`, если каждый файл содержит свой `torrent_id` и `hash`;
- при `requires_selection=false` владельцев нет, поэтому `files` пуст; полный список можно получить через RPC;
- `max_payload_bytes` ограничивает полный список файлов; при превышении возвращается `script payload is too large`.

Symlink-и не разрешаются демоном в `path`: скрипт получает путь из метаданных торрента. Клиент не может влиять на `path`, `download_dir` или `relative_path`.

### 7.6. Оркестраторные скрипты и внешние хосты

Оркестраторный скрипт выполняется на хосте демона и сам обращается к внешним RPC. Runner не проксирует эти запросы, не передаёт клиенту URL внешних хостов и не выполняет копирование между хостами; это логика доверенного скрипта.

Выбранные в параметрах `type=host` ID разрешаются в объект `external_hosts` Execution Payload:

```json
{
  "parameters": {
    "target": "qnap",
    "dry_run": true
  },
  "external_hosts": {
    "qnap": {
      "name": "QNAP",
      "rpc_url": "http://192.168.1.10:9091/transmission/rpc",
      "netrc_machine": "qnap"
    }
  }
}
```

Правила:

- в `external_hosts` попадают только выбранные включённые хосты; дубликаты ID удаляются;
- `parameters.<host_key>` содержит ID или массив ID, а не URL и не netrc machine;
- `rpc_url` и `netrc_machine` берутся только из административного каталога; клиент не может их подменить;
- `rpc_url` не содержит учётных данных; `https` обрабатывается HTTP-клиентом скрипта, локальный RPC runner-а остаётся HTTP-only;
- скрипт использует путь `rpc.netrc` и `external_hosts[id].netrc_machine` для поиска записи `machine` в общем netrc;
- если netrc не настроен, скрипт использует собственный защищённый источник кредов;
- скрипт сам отвечает за таймауты, повторные попытки, частичные ошибки и TLS-проверку внешнего хоста;
- `external_hosts` присутствует всегда; если параметров `type=host` нет или ничего не выбрано, передаётся `{}`.

### 7.7. Дополнительные поля

Демон всегда добавляет `script`:

```json
{
  "script": {
    "id": "torrent-priority",
    "name": "Set file priorities"
  }
}
```

Скрипт не должен считать поля, переданные клиентом, доверенными. Для серверных данных используется `server`, а не произвольный `host`.

Неизвестные поля в Execution Payload должны игнорироваться скриптом. Поля `parameters` и `external_hosts` также присутствуют всегда; их формирование описано в 7.3 и 7.6.

### 7.8. RPC URL и netrc

`rpc.url` формирует демон из своих настроек локального RPC:

- схема всегда `http`; локальный RPC-сервер этого форка не поддерживает TLS;
- адрес: если RPC слушает `0.0.0.0` или `::`, используется `127.0.0.1` или `[::1]`; иначе используется настроенный bind-address;
- порт из `rpc-port`;
- путь из `rpc-url` (по умолчанию `/transmission/rpc`).

Если RPC выключен, `rpc.url` равен `null`, и локальные RPC-действия недоступны. `rpc.url` никогда не содержит логин, пароль или token. URL внешних хостов находятся в `external_hosts` Execution Payload и не заменяют `rpc.url`.

`rpc.netrc` содержит абсолютный путь из `rpc_netrc_file` или `null`, если автоматический netrc не настроен. Один файл может содержать несколько записей `machine` для локального и внешних хостов:

```text
machine 127.0.0.1
  login script-runner
  password <secret>

machine qnap
  login script-runner
  password <secret>

machine samsung-pc
  login script-runner
  password <secret>
```

Для внешнего хоста скрипт ищет запись по `external_hosts[id].netrc_machine`. Демон не читает и не логирует содержимое netrc; он проверяет путь, тип файла и права доступа. Скрипт сам читает netrc и использует его для HTTP Basic аутентификации. Если RPC требует аутентификацию, а netrc не настроен, скрипт должен использовать собственный защищённый источник кредов; иначе RPC-запросы будут отклонены.

Локальный RPC-транспорт в v1 не шифруется. Для runner рекомендуется `rpc-bind-address=127.0.0.1`; скрипты должны обращаться к локальному RPC через loopback. Если RPC доступен за пределами loopback, netrc и `job_token` можно перехватить; это ответственность deployment-а. HTTPS/TLS для локального RPC в v1 не поддерживается: reverse proxy и завершение TLS вне области runner.

Внешние `rpc_url` могут использовать `http` или `https`; TLS-проверку выполняет HTTP-клиент скрипта. Runner не проксирует внешние запросы и не проверяет их TLS.

Полный набор доступных RPC-методов определяется учётной записью из netrc и настройками соответствующего демона.

Демон также передаёт `TR_SCRIPT_RPC_URL` и `TR_SCRIPT_RPC_NETRC` в окружение, если эти значения доступны. Значения не содержат секретов и относятся к локальному RPC.

## 8. Исполнение процесса

### 8.1. Общие правила

- Shell не используется.
- Команда формируется только из `executable` и `args` конфигурации.
- Значения runtime-параметров и параметров `storage=host` передаются только в Execution Payload; они не подставляются в `args` или `env`.
- Execution Payload записывается в stdin процесса в UTF-8 без BOM.
- stdin закрывается после записи; ошибка `EPIPE` не считается ошибкой job.
- stdout и stderr читаются одновременно и независимо, чтобы не допустить взаимной блокировки.
- Рабочий каталог — обязательный `cwd`.
- Переменные окружения — минимальный набор ОС, `TR_SCRIPT_RPC_URL`/`TR_SCRIPT_RPC_NETRC` (если доступны) плюс `env` из конфигурации.
- Код возврата и завершение по сигналу передаются в статус job.
- По таймауту процесс и все его потомки завершаются принудительно.
- По отмене процесс и все его потомки завершаются принудительно.
- Вывод ограничивается отдельно для stdout и stderr; после достижения лимита данные продолжают читаться и отбрасываться.
- Если лидер процесса завершился, stdin закрывается, вывод дочитывается с коротким grace-периодом, оставшиеся потомки завершаются, чтобы не оставлять сирот и не удерживать пайпы открытыми.
- При остановке демона все активные job-ы и их потомки завершаются.
- Restart helper запускается вне очереди jobs, без stdin; его stdout/stderr ограниченно логируются и не возвращаются клиенту.
- Скрипты должны завершаться самостоятельно; фоновые и daemonizing-скрипты не поддерживаются.

### 8.2. Linux

- Использовать `posix_spawn` или `fork`/`exec` без промежуточного shell.
- Создавать отдельную группу процессов.
- В дочернем процессе сбросить унаследованную маску сигналов и восстановить обработку `SIGPIPE`, чтобы демон не влиял на поведение скрипта.
- Закрывать все файловые дескрипторы, кроме stdin/stdout/stderr.
- Наследовать пользователя и группу демона.
- По таймауту и отмене сначала SIGTERM всей группе, затем через 5 секунд SIGKILL.
- Учитывать, что потомок может выйти из группы процессов; полное завершение дерева в этом случае не гарантируется.
- Ограничить окружение и права доступа к временным файлам.
- Restart-планировщик и helper запускать в отдельной сессии/группе процессов, не связывая с группой job, чтобы остановка демона не убивала их до перезапуска.
- Для QNAP использовать тот же POSIX-путь с учётом возможностей старой libc/armv5.

### 8.3. Windows

- Использовать `CreateProcessW` без `cmd.exe`.
- Создавать процесс приостановленным (`CREATE_SUSPENDED`), назначать его в Job Object, затем возобновлять, чтобы не было гонки с созданием потомков.
- Использовать Job Object с завершением дерева процессов; при остановке демона закрывать Job Object с `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
- stdout/stderr подключать через pipes.
- Передавать только необходимые handles; не наследовать handles демона.
- Не показывать консольное окно (`CREATE_NO_WINDOW`).
- Использовать Unicode-пути и `CREATE_UNICODE_ENVIRONMENT`.
- Запрещать `executable` с расширениями `.bat`, `.cmd`, `.ps1` и аналогичные обёртки.
- Restart-планировщик и helper не назначать в Job Object демона; использовать отдельную службу/задачу или процесс с `CREATE_BREAKAWAY_FROM_JOB`, иначе `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` завершит их вместе с демоном.
- Учитывать, что демон может работать как `LocalService` с ограниченным доступом к сети и профилям пользователей.
- Код возврата `DWORD` передавать как целое; для timeout/cancel состояние определяется runner-ом.

### 8.4. Кодировка вывода

Execution Payload кодируется в UTF-8. stdout и stderr декодируются как UTF-8. Некорректные последовательности и NUL заменяются на U+FFFD. Это гарантирует, что ответ RPC всегда является валидным JSON.

## 9. Безопасность

Скрипты являются доверенным административным кодом. Runner не является sandbox: скрипт работает от имени пользователя демона и с полным RPC-доступом может делать всё, что может демон. Граница безопасности — права на `script-runner.json`, `script-runner-values.json`, каталог скриптов и netrc. Если требуются недоверенные скрипты, полный RPC-доступ через netrc недопустим; нужен отдельный механизм ограничения прав.

- `script-runner.json` и каталог скриптов доступны на запись только администратору, системе и пользователю демона; при нарушении runner выключается.
- `script-runner-values.json` и его каталог доступны на запись только администратору, системе и пользователю демона; при нарушении хранилище значений `storage=host` недоступно, но runner продолжает работать.
- Клиент не может передать `executable`, `args`, `cwd`, `env`, `path`, `download_dir`, `rpc` или произвольную команду. Клиент передаёт `hash`, `file_index` и значения только runtime-параметров; значения `storage=host` демон берёт из своего хранилища.
- Клиент не может загрузить новый скрипт через RPC.
- `script_settings_set` меняет только значения параметров `storage=host`; он не может изменить `executable`, `args`, `cwd`, `env`, состав параметров или другие поля `script-runner.json`.
- Все скрипты запускаются только по `id` из конфигурации.
- Нельзя запускать `sh -c`, `cmd /C`, PowerShell-строки и аналогичные обёртки.
- `executable` должен быть абсолютным путём к обычному файлу после разрешения symlink.
- Не допускать запуск world-writable `executable`, `cwd` и файлов из `args` на Unix, если это можно проверить.
- Payload не содержит RPC-логинов, паролей, SSH-ключей, cookie и иных секретов; передаются только `rpc.url`, путь к netrc и выбранные `external_hosts` с их `rpc_url`/`netrc_machine`, но без кредов.
- Runtime-значения передаёт клиент; значения `storage=host` хранятся на демоне и изменяются через `script_settings_set`. Демон проверяет тип, форму и границы, но не доверяет значениям; семантическую проверку выполняет скрипт. Параметры не предназначены для секретов.
- `script_settings_get` и `script_settings_set` доступны всем аутентифицированным RPC-клиентам, как и `script_run`; отдельной административной роли в v1 нет. Если к RPC имеют доступ недоверенные клиенты, они могут изменить значения `storage=host` в пределах объявленных параметров `storage=host`.
- RPC-креды хранятся в netrc; файл доступен на чтение только администратору, системе и пользователю демона. Содержимое netrc не логируется.
- Внешние хосты задаются только администратором в `external_hosts`; клиент выбирает только `id` из разрешённого списка и не может передать URL, `netrc_machine` или иные данные хоста.
- `external_hosts[].rpc_url` и `netrc_machine` не входят в публичный descriptor; они попадают только в Execution Payload выбранного скрипта. URL внешних хостов не логируются.
- Один `rpc_netrc_file` может содержать записи `machine` для локального и внешних хостов; `external_hosts[].netrc_machine` выбирает нужную запись. Права на файл те же, что для локального netrc.
- Скрипты имеют RPC-доступ в пределах прав учётной записи из netrc; `rpc.url` и netrc не дают прав сверх RPC-аутентификации.
- Перезапуск демона — привилегированная операция: helper должен принадлежать администратору/системе, быть недоступным на запись пользователю демона и выполнять только перезапуск, а не произвольные команды.
- Клиент не может передать helper, его аргументы, `cwd` или окружение; RPC-доступ к `script_restart_daemon` должны иметь только административные клиенты.
- Перезапуск ограничен одним запланированным запуском и cooldown; active jobs завершаются вместе с демоном.
- Локальный RPC-транспорт в v1 только HTTP. Скрипты должны обращаться к локальному RPC через loopback; при доступе за пределами loopback netrc, `job_token`, `client_request_id` и `client_secret` могут быть перехвачены. Внешние `rpc_url` могут использовать HTTPS, TLS проверяет скрипт.
- Доступ к `script_status` и `script_cancel` возможен только при предъявлении правильного `job_token`. `job_id` — случайный UUID, но не средство авторизации.
- `client_request_id` и `client_secret` хранятся только в виде SHA-256, сравниваются в постоянное время и не логируются; `job_token` также хранится в виде хэша.
- `client_id` используется только для `queue_limit_per_client` и `max_concurrent_per_client`; его можно подделать, поэтому он не является средством авторизации.
- Вывод ограничивается `max_output_bytes` на поток и `max_total_output_bytes` для завершённых jobs.
- Jobs ограничены по времени, количеству и памяти.
- В логе не логировать payload целиком, значения параметров, URL внешних хостов, `job_token`, `client_request_id`, `client_secret` и содержимое netrc по умолчанию. Для диагностики допустимо логировать имена параметров, количество и ID внешних хостов.
- Вывод restart helper не возвращается клиенту; в лог пишется только факт запуска, код возврата и ограниченный хвост ошибки без секретов.
- Для аудита писать: `job_id`, `script_id`, `client_id`, ID внешних хостов, количество torrents/files, код возврата, сигнал, длительность, факт таймаута, отмены и усечения вывода, текст ошибки запуска; для перезапуска — `restart_id` и `scheduled_at`.
- `confirm` — подсказка клиенту, а не средство безопасности; демон не может гарантировать его выполнение.
- Скрипты выполняются от имени пользователя демона и не должны получать повышенные права.

## 10. Ошибки RPC

В JSON-RPC ошибки возвращаются через `error`; в legacy RPC те же строки попадают в `result`. Для большинства ошибок используется `Error::INVALID_PARAMS`; внутренние сбои — `Error::SYSTEM_ERROR`.

Рекомендуемые строки:

- `script runner is disabled`;
- `script not found`;
- `script is disabled`;
- `script is not available on this platform`;
- `script does not support this context`;
- `script requires a selection`;
- `invalid script payload`;
- `invalid script parameters`;
- `script settings unavailable`;
- `script settings incomplete`;
- `script settings revision conflict`;
- `script selection is too large`;
- `script payload is too large`;
- `script queue is full`;
- `client script queue is full`;
- `client script limit reached`;
- `script request id conflict`;
- `job not found`;
- `invalid output range`;
- `script execution failed`;
- `restart is disabled`;
- `restart already scheduled`;
- `restart cooldown active`;
- `restart helper failed`.

`invalid script parameters` используется для отсутствующего `required`, неизвестного ключа, ключа `storage=host` в `script_run`, неверного типа или формы массива, `null`, объекта, вложенного массива, превышения длины строки или массива и нарушения `min`/`max`; для runtime-параметра `type=host` сюда же входят неизвестный, выключенный или неразрешённый ID. Если параметры увеличивают Request Payload сверх `max_payload_bytes`, возвращается `script payload is too large`.

`script settings unavailable` возвращается, если хранилище значений `storage=host` не читается, невалидно, имеет недопустимые права или не может быть записано. `script settings incomplete` возвращается при запуске скрипта, если обязательный параметр `storage=host` не имеет значения и `default`, либо при `script_settings_set`, если после применения запроса обязательный параметр `storage=host` остаётся пустым. `script settings revision conflict` возвращается, если `expected_revision` не совпадает с текущей ревизией значений `storage=host`; в этом случае значения не меняются. Если хранилище недоступно, возвращается `script settings unavailable`, даже когда обязательные значения отсутствуют; `script settings incomplete` используется только при доступном хранилище.

`invalid script payload` используется для неверной схемы, неизвестного `context`, непустого selection у `context=system`, неверной формы `selection` и нарушения формата `client_id`/`client_request_id`/`client_secret`.

`restart is disabled` возвращается, если feature `script_restart_daemon/v1` отсутствует. `restart already scheduled` — уже есть запланированный перезапуск; `restart cooldown active` — не истёк cooldown; `restart helper failed` — не удалось создать планировщик или подготовить запуск helper-а. Ошибки запуска и работы helper-а после ответа не возвращаются клиенту, а логируются.

`script execution failed` используется для внутренней ошибки до создания job. Ошибки уже созданного job передаются через `script_status` в полях `state`, `error`, `exit_code` и `term_signal`, а не как RPC-ошибка. `script timed out` не является RPC-ошибкой: это состояние `timed_out`. `job not found` также возвращается при неверном или отсутствующем `job_token`.

Для ошибок конфигурации использовать отдельные сообщения в логе; в RPC не раскрывать внутренние пути и команды.

## 11. Ограничения и хранение jobs

- `max_concurrent` ограничивает число одновременно запущенных процессов.
- `queue_limit` ограничивает число ожидающих jobs.
- При переполнении общей очереди `script_run` возвращает `script queue is full`.
- `queue_limit_per_client` ограничивает число ожидающих jobs для одного `client_id`; при переполнении возвращается `client script queue is full`.
- `max_concurrent_per_client` ограничивает число одновременно выполняемых jobs для одного `client_id`; при превышении возвращается `client script limit reached`.
- Если `client_id` не задан, используется общий bucket `default`; `client_id` не является средством авторизации.
- `max_selection_items` ограничивает суммарное число выбранных торрентов и файлов.
- На один скрипт допускается не более 64 параметров; строка параметра — не более 4096 байт; массив — не более 256 элементов.
- `script-runner-values.json` имеет размер не более 1 MiB, содержит не более 256 скриптов и не более 64 параметров `storage=host` на скрипт.
- Каталог `external_hosts` содержит не более 64 хостов; эффективный список параметра `type=host` не больше каталога.
- `max_payload_bytes` ограничивает Request Payload и сформированный Execution Payload, включая значения runtime-параметров и параметров `storage=host` и данные выбранных внешних хостов.
- Одновременно допускается один запланированный перезапуск; между принятыми перезапусками действует `cooldown_seconds`.
- Перезапуск завершает активные jobs вместе с демоном; статус jobs после рестарта недоступен.
- `max_output_bytes` ограничивает отдельно stdout и отдельно stderr каждого job.
- `max_retained_jobs` ограничивает число завершённых jobs.
- `max_total_output_bytes` ограничивает суммарный сохранённый вывод завершённых jobs.
- Завершённые jobs хранятся `job_retention_seconds`.
- При превышении `max_retained_jobs` или `max_total_output_bytes` удаляются самые старые завершённые jobs, даже если retention ещё не истёк.
- Клиент должен своевременно читать вывод: eviction может удалить завершённый job раньше `job_retention_seconds`.
- Активные и ожидающие jobs не удаляются по этим лимитам.
- Старые jobs удаляются автоматически.
- После перезапуска демона jobs не восстанавливаются; запрос к старому `job_id` возвращает `job not found`.
- Пара `client_request_id`/`client_secret` действует, пока соответствующий job хранится; после eviction или retention ключ может быть использован повторно.
- `job_token` действует, пока job хранится; после eviction, retention или перезапуска демона он недействителен.
- При остановке демона активные jobs завершаются; их состояние после рестарта недоступно.
- `script_status` отдаёт вывод страницами; размер одной страницы по умолчанию 64 KiB на поток, максимум 1 MiB на поток за запрос.

## 12. Совместимость

- `script_runner/v1` является опциональной возможностью.
- `script_runner_settings/v1` является дополнительной feature для `script_settings_get`/`script_settings_set`; клиент обязан проверять её перед показом вкладки host-настроек.
- Клиент обязан проверять feature перед показом меню.
- Неизвестные поля верхнего уровня в payload и descriptor должны игнорироваться; неизвестные ключи внутри `parameters` отклоняются.
- ID скриптов стабильны; изменение ID считается новым скриптом.
- Публичные поля descriptor нельзя удалять без повышения версии feature.
- `parameters` являются публичными descriptor-полями; значения runtime-параметров и параметров `storage=host` в descriptor не входят.
- `storage` входит в descriptor и `revision`; отсутствие `storage` означает `runtime`. Значения параметров `storage=host` и ревизия значений `storage=host` не входят в descriptor и не меняют его `revision`.
- Добавление необязательного параметра совместимо. Добавление обязательного параметра, удаление параметра, изменение `key`, `type`, `array` или `required` с `false` на `true` ломает старых клиентов и требует `v2`. Изменение `default`, `min` или `max` меняет `revision` и поведение, но не формат протокола. Изменение `storage` с `runtime` на `host` или обратно меняет набор ключей, принимаемых `script_run`, и требует `v2`.
- Клиент обязан обновлять форму параметров при изменении `revision` и передавать только runtime-ключи из текущего descriptor.
- Клиент без поддержки `storage` может показать параметр `storage=host` в форме запуска; такой запуск получит `invalid script parameters`. Для полноценной работы с параметрами `storage=host` требуется клиент с `script_runner_settings/v1`.
- `external_hosts` в descriptor содержит публичные `id`, `name` и `description`; `rpc_url` и `netrc_machine` приватны и не влияют на `revision`.
- Добавление внешнего хоста совместимо; удаление или переименование хоста меняет `revision` и может сломать скрипты и сохранённые настройки клиента, поэтому требует отдельного `id` или повышения версии feature.
- `type=host` является частью `transgui.script/v1`; добавление необязательного параметра `type=host` совместимо, обязательного — требует `v2`.
- Значения `storage=host` хранятся только на демоне в `script-runner-values.json` и не переносятся в `transgui.ini`; каждый хост имеет собственный набор значений.
- `script_settings_get`/`script_settings_set` используют отдельную ревизию значений `storage=host`; изменение значений не меняет `revision` descriptor-а и не требует перезапуска демона.
- `system` — дополнительный context `transgui.script/v1`; старые клиенты могут игнорировать такие записи, новые запускают их с пустым selection. `system` не комбинируется с `torrent`/`file`.
- `script_restart_daemon/v1` — опциональная feature; её наличие и `restart_daemon.enabled` входят в descriptor и `revision`, а helper и его аргументы — нет.
- Request Payload и Execution Payload `transgui.script/v1` нельзя менять несовместимым образом; для изменений вводится `v2`.
- В v1 скрипты получают `rpc.url`, путь к netrc и выбранные `external_hosts`; изменение контракта RPC-доступа требует повышения версии.
- Локальный RPC-транспорт в v1 только HTTP; HTTPS/TLS локального RPC через reverse proxy не входит в v1. Внешние `rpc_url` могут использовать HTTPS.
- `rpc.url`, путь к netrc, `job_token`, `client_request_id` и `client_secret` не входят в публичный descriptor и не влияют на `revision`.
- v1 поддерживает Linux и Windows. На других ОС feature отсутствует.
- `torrent_id` не является стабильным идентификатором; для совместимости скрипты должны использовать `hash`.
- `revision` меняется при изменении публичного descriptor; client может кешировать меню по `revision`.

## 13. Примеры конфигурации для PHP

### Linux

```json
{
  "id": "torrent-priority",
  "name": "Set file priorities",
  "contexts": ["torrent"],
  "parameters": [
    {"key": "priority", "name": "Priority", "type": "integer", "storage": "runtime", "default": 0, "min": -1, "max": 1},
    {"key": "dry_run", "name": "Dry run", "type": "boolean", "storage": "runtime", "default": false}
  ],
  "payload": "transgui.script/v1",
  "stdin": "json",
  "executable": "/usr/bin/php",
  "args": ["/opt/transgui/scripts/priority.php"],
  "cwd": "/opt/transgui/scripts",
  "timeout_seconds": 300
}
```

### Windows

```json
{
  "id": "torrent-priority",
  "name": "Set file priorities",
  "contexts": ["torrent"],
  "parameters": [
    {"key": "priority", "name": "Priority", "type": "integer", "storage": "runtime", "default": 0, "min": -1, "max": 1},
    {"key": "dry_run", "name": "Dry run", "type": "boolean", "storage": "runtime", "default": false}
  ],
  "payload": "transgui.script/v1",
  "stdin": "json",
  "executable": "C:\\php\\php.exe",
  "args": ["C:\\ProgramData\\TransGUI\\scripts\\priority.php"],
  "cwd": "C:\\ProgramData\\TransGUI\\scripts",
  "timeout_seconds": 300
}
```

PHP-скрипт читает Execution Payload одинаково в обоих случаях:

```php
$data = json_decode(stream_get_contents(STDIN), true, 512, JSON_THROW_ON_ERROR);
$parameters = $data['parameters'] ?? [];
$priority = $parameters['priority'] ?? 0;
$dryRun = $parameters['dry_run'] ?? false;
```

PHP-скрипт получает `rpc.url` и путь к netrc в Execution Payload, а также `TR_SCRIPT_RPC_URL`/`TR_SCRIPT_RPC_NETRC` в окружении. Пароль в payload не передаётся; скрипт читает его из netrc. Значения в `parameters` уже проверены демоном; `default` применены, значения `storage=host` добавлены из `script-runner-values.json`, а для необязательных параметров без `default` ключ может отсутствовать.

## 14. Что не входит в область

- Клиентские скрипты, запускаемые на машине пользователя.
- Автоматическое обнаружение и регистрация внешних хостов; каталог задаёт только администратор.
- Проксирование RPC и передача файлов между хостами силами runner; этим занимается оркестраторный скрипт.
- Настройка sudoers, polkit, ACL, службы или задачи для restart helper; это deployment-ответственность администратора.
- Загрузка и импорт новых скриптов через RPC.
- Изменение административной конфигурации `script-runner.json` через RPC, включая состав параметров и пути к исполняемым файлам.
- Хранение параметров `storage=host` в `transgui.ini` или других клиентских файлах; значения `storage=host` принадлежат демону.
- SSH-транспорт к удалённому хосту. Он может быть отдельной клиентской реализацией.
- Изменение legacy-меню `%s` в TransGUI.
- Ограничение набора RPC-методов на стороне runner; доступ определяется учётной записью из netrc.
- Sandbox и изоляция скриптов; скрипты считаются доверенным административным кодом.
- HTTPS/TLS для локального RPC и reverse proxy; локальный RPC v1 использует только HTTP. TLS внешних хостов — на стороне скрипта.
- Поддержка macOS, Haiku и других ОС, кроме Linux и Windows.
- Фоновые, daemonizing- и долгоживущие скрипты.
- Управление содержимым netrc и секретами скриптов.

## 15. Критерии приёмки

1. Валидная конфигурация загружается, невалидные скрипты пропускаются с записью в лог.
2. Невалидный файл конфигурации или слишком широкие права выключают runner; feature `script_runner/v1` отсутствует.
3. При включённом runner на Linux/Windows `fork-features` содержит `script_runner/v1` и `script_runner_settings/v1`.
4. `session-get` возвращает только публичные descriptor-поля, включая `storage`; `revision` стабилен для одинаковой конфигурации и меняется при изменении публичных полей, но не при изменении значений `storage=host`.
5. Скрипт с контекстом `torrent` получает корректный Execution Payload для одного и нескольких торрентов, разрешённых из хешей.
6. Скрипт с контекстом `file` получает все файлы владельцев с `wanted`, `priority` и `selected`, а не только выбранные; `download_dir` является базой, `relative_path` от неё, `path` фактический.
7. Скрипт с `requires_selection=false` запускается с пустым набором.
8. Payload с неверной схемой, неверным контекстом, неизвестным хешем, неверным `file_index` или чужим файлом отклоняется; превышение `max_selection_items` и `max_payload_bytes` также отклоняется.
9. Клиент не может передать `executable`, `args`, `cwd`, `env`, `path`, `download_dir`, `rpc`, URL/`netrc_machine` внешнего хоста или иные серверные данные; в `script_run` разрешены только runtime-ключи из descriptor; значения `storage=host` берутся только из хранилища демона; демон формирует серверные поля сам.
10. Таймаут завершает процесс и всех потомков.
11. Превышение лимита вывода не приводит к зависанию; `stdout_truncated`/`stderr_truncated` выставляются, offset/limit работают.
12. Одновременные запуски соблюдают `max_concurrent` и `queue_limit`.
13. `script_cancel` завершает queued и running job, переводит их в `canceled` и завершает дерево процессов.
14. Пара `client_request_id`/`client_secret` возвращает существующий job и `job_token` при повторе; неверный секрет или другой payload дают `script request id conflict`.
15. `SIGHUP`/`systemctl reload` атомарно применяет конфигурацию; невалидная конфигурация выключает runner, запущенные jobs дорабатывают, ожидающие отменяются.
16. При остановке демона активные jobs и их потомки завершаются; после рестарта jobs недоступны, конфигурация читается заново.
17. `script_status` и `script_cancel` требуют правильный `job_token`; без него или с чужим токеном возвращается `job not found`.
18. На Linux и Windows скрипт с одинаковым Execution Payload работает через `php.exe`/`/usr/bin/php` без shell; `cwd` обязателен.
19. `max_retained_jobs` и `max_total_output_bytes` удаляют самые старые завершённые jobs и не удаляют активные.
20. При выключенном runner меню скриптов у клиента отсутствует.
21. Execution Payload содержит `rpc.url`, путь к netrc и выбранные `external_hosts`; скрипт может выполнить локальный и внешний RPC-запрос с полным доступом учётной записи; логин и пароль в payload отсутствуют.
22. Netrc с слишком широкими правами выключает runner; содержимое netrc, `job_token`, `client_request_id`, `client_secret` и значения параметров не попадают в лог.
23. `queue_limit_per_client` и `max_concurrent_per_client` соблюдаются для каждого `client_id`; при отсутствии `client_id` используется bucket `default`.
24. `rpc.url` локального RPC всегда использует схему `http`; TLS/HTTPS локального RPC в v1 отсутствует, а внешние `rpc_url` могут использовать `https`.
25. Descriptor содержит `parameters` с `key`, `name`, `type`, `storage`, `array`, `required` и заданными `default`/`min`/`max`; изменение описаний или `storage` меняет `revision`.
26. Клиент может передать скалярные и массивные runtime-значения `string`, `integer`, `number` и `boolean`; `default` применяются, необязательные параметры без `default` отсутствуют, а Execution Payload содержит разрешённые runtime-значения и значения `storage=host` в `parameters`.
27. Отсутствующий `required` runtime-параметр, неизвестный ключ, ключ `storage=host` в `script_run`, неверный тип или форма массива, `null`, объект, вложенный массив, строка длиннее 4096 байт или массив длиннее 256 элементов дают `invalid script parameters`. Отсутствие обязательного параметра `storage=host` даёт `script settings incomplete` при доступном хранилище и `script settings unavailable` при недоступном.
28. Некорректное описание параметра (дубликат `key`, неверный `storage`, `required` вместе с `default`, неверный `default`, `min > max`) делает скрипт невалидным, не затрагивая остальные скрипты.
29. Descriptor содержит только публичные поля включённых `external_hosts` (`id`, `name`, `description`), а параметры `type=host` — эффективный список `hosts`; `rpc_url` и `netrc_machine` в descriptor отсутствуют.
30. Параметр `type=host` принимает только существующие включённые ID из эффективного списка; Execution Payload содержит выбранные хосты в `external_hosts`, и скрипт может обратиться к ним через общий netrc.
31. Клиент не может передать URL, `netrc_machine` или иные данные внешнего хоста; попытка подменить `external_hosts` в Request Payload игнорируется.
32. `restart_daemon` в descriptor содержит `id`, `name`, `context: "system"` и `enabled`; при `enabled=false` feature `script_restart_daemon/v1` отсутствует, а метод возвращает `restart is disabled`.
33. При включённом перезапуске helper запускается после `delay_seconds`, ответ содержит `accepted`, `restart_id` и `scheduled_at`; повторный запрос и cooldown дают соответствующие ошибки.
34. Невалидная конфигурация `restart_daemon` выключает runner; helper с неверными правами отклоняется; активные jobs завершаются вместе с демоном.
35. На Windows helper является отдельным `.exe`, а не `.bat`/`.cmd`/`.ps1`; перезапуск не полагается на несуществующую команду `sc restart`.
36. Скрипт с `contexts: ["system"]` запускается с пустым selection и получает пустые `torrents`/`files`; сочетание `system` с `torrent`/`file` и `requires_selection=true` для `system` делают скрипт невалидным; restart daemon не входит в `scripts`.
37. Параметры `storage=host` хранятся в `script-runner-values.json` отдельно для каждого хоста; файл отсутствует или содержит значения; runtime-значения в нём игнорируются.
38. `script_settings_get` возвращает только включённые скрипты текущей платформы с параметрами `storage=host`, эффективные значения, `ready` и `missing`; значения не попадают в `session-get.script_runner` и не меняют `revision` descriptor-а.
39. `script_settings_set` атомарно сохраняет изменения, проверяет `expected_revision`, типы, `required`, `min`, `max` и `hosts`; runtime-ключи и неизвестные ключи отклоняются; ошибка записи не портит старый файл.
40. Новая host-настройка применяется к новым jobs; queued и running jobs используют снимок значений, взятый при постановке в очередь.
41. В окне «Параметры Transmission» есть вкладка «Параметры скриптов» с параметрами `storage=host` текущего хоста; runtime-параметры в ней отсутствуют; OK сохраняет через `script_settings_set`, Cancel не сохраняет; значения не пишутся в `transgui.ini`.
42. Если хранилище доступно, но обязательный параметр `storage=host` не настроен, скрипт остаётся видимым, но `script_run` возвращает `script settings incomplete`, а клиент показывает ошибку и не запускает job.
43. При недопустимых правах или невалидном `script-runner-values.json` хранилище значений `storage=host` недоступно, `script_settings_get/set` возвращают `script settings unavailable`, но runtime-скрипты продолжают работать.
44. `script_runner_settings/v1` присутствует при включённом runner; без неё клиент не показывает вкладку host-настроек.
