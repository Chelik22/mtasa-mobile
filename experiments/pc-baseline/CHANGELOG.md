# Changelog

## Unreleased

- Подготовлен lab-only native user-directory hook для одного SHA-проверенного GTA
  EXE: exact full-image whitelist штатных loader patches, ACP/256-byte guards, три native
  буфера и процессный detour с проверкой инструкций. Source anchor подключён
  до net/Core startup с actual process-name guard для CEF; game/GUI и полная
  native registry isolation остаются непроверенными.

- Исправлена область client-only guard в Windows SDK file routing: server TUs
  не должны ссылаться на lab namespace. Добавлен compile-only test настоящего
  `SharedUtil.File.hpp` без `MTA_CLIENT`; helper-only executable эту ошибку не ловил.
- **Отдельное пространство PC lab paths.** Добавлен явный режим с файловыми
  MTA settings и локальными app/game/data путями, чтобы подготовка собственного
  клиента не меняла установленные игры, реестр и системные repair. Простой copy
  launcher folder отвергнут: штатный loader всё равно использовал common registry,
  ProgramData и Documents. Контракт/границы и реальный Win32 filesystem proof
  описаны в `lab/README.md`; native GTA/SDK routing/gameplay ещё не проверены.
- SDK preflight сравнивает полный frozen/current file map, включая добавленные
  shadowing headers. Regression проверяет отказ реального build preflight до compiler.
- Подготовлены pinned dependency cache, source-only настоящий DxErr archive и
  изолированный Win32 build plan с проверенными MSBuild targets, PID/log/lock,
  ограничением одного worker и сохранением честного `G1=not_run`.
- Проверены реальные installed GTA/MTA paths и статический GTA fingerprint через
  REA; игра, UI, полноценный клиент MTA и production RP не запускались.
