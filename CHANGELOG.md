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

### Fixed

- Archive entries keep the display names stored in their `.resume` files.
- Reduced memory pressure by capping pending writes and applying backpressure.
