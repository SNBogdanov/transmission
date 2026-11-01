# Changelog

## Unreleased

### Added

- Archive RPC (`torrent_archive`) for saving, restoring, batch restoring,
  moving, exporting, importing, creating, deleting, and listing archives.
- Archive list sorting by name, size, added date, archived date, and folder,
  with ascending/descending order and label filtering.
- `torrent_archive.delete` to remove one archived entry.
- `torrent_archive.view` to return a ZIP bundle with an entry's `.torrent` and
  `.resume` files; `download` is accepted as an alias.
- Per-torrent `incomplete_dir` and the `torrent_set_incomplete_dir` RPC method.
- `min_free_space` and `min_free_space_resume_margin` session settings.
- Move-completed/partial-file, script-torrent-done-remove, and
  force-finish-partial-files settings.
- Auto-priority per torrent (`auto_priority`) with session defaults
  `auto_priority_enabled`, `auto_priority_max_high`, and `auto_priority_sort`.
  Wanted incomplete files are sorted by id, name, or size and split into
  thirds, with the high-priority group capped by `auto_priority_max_high`;
  a single remaining file stays normal. Unwanted and completed files stay
  normal. Manual file-priority changes disable it until it is re-enabled via
  `torrent-set`. Changing `auto_priority_max_high` or `auto_priority_sort` via
  `session-set` immediately re-applies priorities to torrents that have
  `auto_priority` enabled.

### Fixed

- Archive entries keep the display names stored in their `.resume` files.
- Torrents paused by the minimum-free-space guard keep their run intent across
  daemon restarts.
- Completed torrents are no longer reported as seeding (status `6`) or
  finished (`isFinished`) until the move to the download directory finishes,
  so download clients do not import files too early. Seeding is paused during
  the move and resumed afterwards.
- Reduced memory pressure by capping pending writes and applying backpressure.
