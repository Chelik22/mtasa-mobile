# Проверка fixture, 2026-10-28

| Проверка | Результат | Граница вывода |
|---|---|---|
| `python experiments/mobile-world-fixture/validate.py` | passed | XML parsed, оба script файла найдены, безопасные относительные пути |
| `git diff --check` / `git diff --cached --check` | passed | Нет whitespace errors |
| Lua 5.1 syntax: `server.lua` | passed | Только загрузка/компиляция chunk, без исполнения |
| Lua 5.1 syntax: `client.lua` | passed | Только загрузка/компиляция chunk, без исполнения |
| Реальный MTA server + два PC | not_run | Транспорт и игровые клиенты не проверены |
| PC + Android, streaming, водитель/пассажир | not_run | Никаких утверждений об общем мире |
| Runtime ACL, spoof, timeout, restart checks | not_run | В README записаны будущие критерии |
| Multitouch / Android lifecycle | not_run | В backend audit записан обязательный R1/A3 gate |

В PATH и `D:/bin` отсутствовал готовый Lua CLI. Для syntax-only проверки из уже
имеющихся `vendor/lua/src` собран временный checker установленным Zig C compiler.
Его main создаёт state через MTA `luaL_newstate(NULL)`, вызывает `luaL_loadfile`
для каждого файла и освобождает state. `lua_pcall` не вызывается. Checker не
загружает MTA runtime, GTA или native game binaries и не исполняет Lua-код.

Использованы core translation units `lapi`, `lauxlib`, `lcode`, `ldebug`, `ldo`,
`ldump`, `lfunc`, `lgc`, `llex`, `lmem`, `lobject`, `lopcodes`, `lparser`, `lstate`,
`lstring`, `ltable`, `ltm`, `lundump`, `lvm`, `lzio` с `-O0 -I vendor/lua/src`.
Первый build checker был исправлен после обнаружения дополнительного аргумента
`mtasaowner` в MTA Lua. Финальный build и обе syntax-проверки завершились code 0.
Предупреждение `-Wempty-body` находится в неизменённом upstream `lauxlib.c:577`.
Новых утилит и библиотек из сети не установлено.
