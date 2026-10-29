# Changelog

## Unreleased

- Native build001: host CTest1/1 и ARM64/API28 final shared link/import/export
  checks passed. Source/log/artifact hashes сохранены, Android/game not_run.

- Добавлен fresh-directory Windows CLI runner: hash/PID/log/exit guards, host
  boundary tests и ARM64/API28 final shared ELF/import/export inspection.
  Diagnostic link probe не является игровым JNI backend.

- Реализован version-bound Android read-only position candidate: фактический
  module/file binding, pinned SHA, live code anchor, GOT selector и kernel copy.
  Добавлены failure-clearing boundary tests. Runtime/frame/lifetime ещё не
  проверены, gameplay position API не включён.
