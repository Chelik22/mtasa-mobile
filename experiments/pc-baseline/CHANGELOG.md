# Changelog

## Unreleased

- Исправлен exact native identity path: SDK хранит launch EXE в
  `data/1.7/GTA San Andreas`, а guard ошибочно ожидал путь без версии и завершил
  первый game process с0x4C414250. Теперь версия выводится из того же compile-time
  ASE version, что и `GetMajorVersionString`; unversioned/other-version пути
  по-прежнему отклоняются. CLI regression не запускает игру или GUI.

- Добавлен отдельный integration plan/runner для проверенного PC source fork в
  существующем cache: baseline immutable, explicit reviewed source delta, SDK/
  project/input guards, Normal one-worker CLI и самостоятельный integration receipt.

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
