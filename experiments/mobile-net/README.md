# Проверка готовности сетевого транспорта

Изолированный эксперимент: сервер ENet и два отдельных клиентских процесса обмениваются ограниченными сообщениями через loopback. Один клиент отключается и подключается повторно с новой generation. Это **не общий игровой мир**, не реализация `CNet`/`CNetServer` и не проверка сохранения Lua RP.

## Зависимости и сборка

Нужны CMake 3.24+, C++17 compiler и Python 3 для запуска процессов и записи evidence. Новые Python packages не требуются. CMake скачивает исходники официального [lsalzman/enet](https://github.com/lsalzman/enet) в каталог сборки:

- версия `v1.3.18`, разрешённая через официальный GitHub API;
- commit `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`;
- SHA256 архива `37381831c5ccbb41c3c71e867203fee847670f67717f297f930290d18985ad5f`;
- MIT license: исходный `LICENSE` остаётся в загруженном дереве ENet; включать его при распространении ENet/binaries.

Из корня репозитория в PowerShell:

```powershell
cmake -S experiments/mobile-net -B experiments/mobile-net/build -G "Visual Studio 18 2026" -A Win32
cmake --build experiments/mobile-net/build --config Release --parallel 2
ctest --test-dir experiments/mobile-net/build -C Release --output-on-failure --parallel 1
```

Обычные CMake generators на Linux тоже допустимы, но считать платформу проверенной можно только после реального выполнения тестов на ней. Первый configure требует доступа к codeload.github.com. Не использовать floating tag вместо зафиксированного commit/hash.

## Контракт сообщения

Все числа беззнаковые, big-endian, без сериализации native structs и указателей.

| Offset | Bytes | Поле |
| --- | --- | --- |
| 0 | 4 | ASCII magic `MNET` |
| 4 | 2 | version, сейчас `1` |
| 6 | 2 | type: hello=1, probe=2, ack=3 |
| 8 | 8 | session, ненулевой идентификатор тестовой установки |
| 16 | 4 | generation, ненулевая версия соединения |
| 20 | 4 | sequence, ненулевой номер сообщения |
| 24 | 4 | длина payload |
| 28 | 0..1024 | payload bytes |

Декодер проверяет magic, version, type, ненулевые поля, точную длину и границу 1024 до выделения памяти под payload. Префиксы и дополнительные байты отклоняются. ENet host также ограничивает packet size и waiting data. В прототипе один ENet channel и только `ENET_PACKET_FLAG_RELIABLE`.

Session — идентификатор теста, **не аутентификация, не официальный MTA serial и не античит**. Сервер ограничен двумя session identities и четырьмя ENet peers. Hello устанавливает generation; повторная generation и совпадающая активная session отклоняются. Probe принимается только с session/generation текущего peer и следующим sequence. Это ограниченные правила тестового harness, а не полный connection protocol.

## Проверка и evidence

`wire_contract` сравнивает кодирование и декодирование с независимым литеральным golden vector; проверяет все его обрезанные префиксы, неверные поля, лишние байты, нулевые идентификаторы, максимальную и превышенную длину payload. Проверки работают и в Release, без `assert`.

`enet_three_process_loopback` запускает настоящий ENet server и **два отдельных client processes**. Два клиента имеют разные случайные session IDs; первый проходит два подключения (generation 1/2), второй одно. В каждом подключении проходят hello и probe с проверкой ответа. Сервер подтверждает 3 подключения, 3 отключения, 6 сообщений, 2 sessions и 1 reconnect. Клиенты соединяются только после подтверждённого bind сервера на `127.0.0.1`; окна и рабочий стол не используются.

Результат: `build/evidence/loopback.json`, JSONL stdout и stderr каждого процесса. До запуска процессов предыдущий результат атомарно заменяется состоянием `running`, `passed=false`; финальное состояние — `passed` либо `failed`. JSON содержит UTC, dependency commit, SHA256 исполняемого probe, PIDs, exit codes, raw stdout/stderr, разобранные сообщения и ошибки разбора. Незавершённый JSON от упавшего процесса сохраняется как raw output и parse error, а не мешает записи провала. Ошибка/таймаут завершает только запущенные тестом процессы и даёт failing test. Вывод не объявляет прохождение MTA/world gate.

`smoke_failure_evidence` запускает маленький падающий probe с не-JSON строкой и оборванным JSON, предварительно записав устаревший успешный результат. Проверяются ненулевой exit, начальное `running/passed=false`, сохранение raw output и parse errors, окончательный durable failure без старого успеха. Второй случай проверяет отсутствие executable ещё до запуска процесса.

## Что пока не поддерживается

- ABI exports, loader integration и все услуги `CNet`/`CNetServer`.
- MTA bitstream, packet IDs, join verification, player elements и sync ownership.
- Все пять MTA reliability modes, четыре ordering channels и priority scheduling: mapping ещё не разработан.
- Download manager, запуск/остановка ресурсов, plain Lua и compiled/obfuscated scripts.
- Античит, аутентификация, шифрование и deployment на публичный сервер.
- Loss/reorder/duplication impairment, LAN/Internet, Android NDK/ARM64 и настоящая GTA.

Следующий gate — проверяемый ABI/bitstream contract выбранного MTA commit и загрузка экспериментального модуля. Затем настоящий сервер, source Lua resource, один PC и два PC. Тест сетевого harness не заменяет эти этапы.
