# Changelog

## [Unreleased]

### Added

- **Изолированная проверка сетевого транспорта.** Добавлены versioned bounded envelope, независимый golden vector и настоящий ENet loopback с двумя клиентскими процессами и reconnect. Ранее в эксперименте не было исполняемой проверки транспорта; ENet выбран вместо самописной надёжной доставки UDP. Реализация и команды проверки находятся в этом каталоге; она не изменяет production MTA network и не подтверждает игровой мир или Lua совместимость.
- **Те же sources собираются NDK.** Добавлены ARM64/x86_64 cross-build без host Python/Android CTest, проверка ELF и hashes pinned ENet cache до configure. Три host test definitions сохранены. Сборка и static inspection пройдены; Android execution и host↔Android traffic ещё не проверены.

### Fixed

- **Провал теста больше не оставляет старый успешный evidence.** Runner заранее записывает `running/passed=false`, а финализация сохраняет raw stdout/stderr и parse errors вместо строгого разбора повреждённого вывода. Раньше оборванный JSON мог прервать финальную запись и оставить предыдущий `passed=true`. Добавлена процессная regression-проверка crash и отсутствующего executable.
