# Changelog

## Unreleased

- SDK preflight сравнивает полный frozen/current file map, включая добавленные
  shadowing headers. Regression проверяет отказ реального build preflight до compiler.
- Подготовлены pinned dependency cache, source-only настоящий DxErr archive и
  изолированный Win32 build plan с проверенными MSBuild targets, PID/log/lock,
  ограничением одного worker и сохранением честного `G1=not_run`.
- Проверены реальные installed GTA/MTA paths и статический GTA fingerprint через
  REA; игра, UI, полноценный клиент MTA и production RP не запускались.
