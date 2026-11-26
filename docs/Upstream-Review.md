# Upstream review

Проверка изменений upstream (`transmission/transmission`) относительно форка.

## 2026-11-26

- Upstream tip: `e024d47a5` — `fix(qt): keep the selected activity filter on Qt 5 (#9140)`, 2026-11-09.
- Форк отставал на 16 коммитов (`HEAD..origin/main`), опережал на 29.
- Решение: полный merge upstream не делать (сильное расхождение в `torrent.cc`, `session.cc`,
  `rpcimpl.cc`, `torrent-files.*`), забрать точечно только нужные исправления.
- Тесты upstream не переносились (решение пользователя).

### Взято

| Коммит | Изменение | Файлы |
|---|---|---|
| `4946b371d` | `sorted_by_path()` сортировал только пути (`std::ranges::sort(std::views::keys(ret))`), из-за чего размеры оставались привязаны к чужим путям. Теперь сортируются пары `std::ranges::sort(ret)`. | `libtransmission/torrent-files.h` |
| `74de67425` | Присваивание `tr_variant` из собственной строки освобождало память и читало из неё (use-after-free). Заменено `val_.emplace<std::string>(...)` на `val_ = std::string{...}`. | `libtransmission/variant.h` |
| `264ce1798` | Не отправлять `Referer` при редиректах webseed-фрагментов: некоторые хосты игнорируют `Range` и отдают файл целиком, если есть cross-site `Referer`. Добавлен `CURLOPT_AUTOREFERER = 0` для webseed. | `libtransmission/web.cc` |

### Пропущено

| Коммит | Причина |
|---|---|
| `c1a0dbebd` | Новый C API `tr_torrentStatAsync()` — нужен для асинхронных статов в macOS-клиенте, форку не требуется. |
| `2fd70b982`, `0b58b2bdb` | Производительность macOS-клиента (Objective-C). |
| `b023460dc` | Безопасность десериализации групповых настроек в macOS. |
| `a350ef0d4`, `0e4d5fb83`, `7c10637f6` | Web UI: generated files, диалоги добавления/переименования/меток, relative add folder. |
| `e024d47a5` | Qt5: сохранение фильтра активности. |
| `a648c2935` | Обновление Sparkle до 2.10.1 (macOS). |
| `5a341c2d0` | Удаление устаревшего QuickLook-плагина (macOS). |
| `e4cf5e578` | Минимальная версия macOS 12 (Xcode 27). |
| `9513a9c0c` | CI: Android SDK, удаление Debian 11. |
| `f4bd5f345` | Тесты: fuzz-тест benc-парсера (тесты не переносим). |
