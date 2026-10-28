# Настоящий headless protocol client для H1

CLI подключается через собственный MTAX/ENet transport к настоящему source-built
MTA fork server. Это не GTA-клиент, не Lua VM и не реализация native world.
До запуска на настоящем сервере H1 остаётся `not_run`, даже если codec tests PASS.
Официальный `netc`/RakNet wire format здесь не поддерживается.

Actual H1 run `20261028T000420Z-4d6737ba` прошёл на настоящем source-built MTA:
player IDs10/11→13, один vehicle12/model411, три accepted Lua roundtrips,
reconnect и очистка до нуля, все exits0. Независимый raw-log/hash/PID аудит PASS.
[Evidence](verification/windows-x64-h1.json). GTA, client Lua VM и Android
этим не проверены.

## Сборка

Root CMake должен собрать `headless/main.cpp` с `mta_bitstream`, ENet и include
directory текущего `experiments/net-module` (для единственного `Protocol.h`).
Собственный Protocol.h не копируется и не форкается. Для Windows дополнительно
нужны те же `ws2_32`/`winmm`, что у ENet transport. Существующие MTA header paths
наследуются от `mta_bitstream`. Новых зависимостей нет.

Перед сборкой: `python experiments/net-module/headless/check_source_constants.py --check`.
Проверка сверяет значения RPC, entity/game и Lua types с authoritative enums
данного checkout. `--write` обновляет малый generated `SourceConstants.h` после
осознанного изменения source protocol. Packet IDs берутся прямо из `net/Packets.h`,
версии — из `version.h`, ElementID — через реальные MTA bitstream helpers.

`codec_tests.cpp` проверяет выбранный packet boundary без сети: join-data writer
против порядка реального server reader, joined-game parser, Lua event/ACK с
реальной element-reference формой и обрезанные event payloads. Это не H1.

## CLI и команды без GUI/stdin thread

На отдельном локальном test runtime, когда experimental module и сервер готовы:

```powershell
headless_client.exe --host 127.0.0.1 --port 22003 --identity AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA --nickname H1_A --duration 120 --commands D:/temp/h1-a.commands
```

Второй процесс использует другой identity, nickname и command file. Identity —
32 uppercase hex characters, prototype identifier; не аппаратный serial и не
доказательство authentication. Требуется сервер без пароля: клиент отправляет
нулевой password hash. Ник ограничен ASCII letters/digits/underscore/hyphen.
Команда `--help` описывает параметры без подключения к сети.

В command file добавляются полные строки с newline:

```text
status
disconnect
connect
reconnect
quit
```

Файл append-only, максимум 64 KiB; недописанная строка ждёт newline. Не обнулять
файл во время работы: truncation считается ошибкой. Client сам файл не создаёт.
Отдельного input thread нет, поэтому завершение не ждёт заблокированного stdin.
`--duration` ограничивает весь процесс (1..3600 секунд), затем запрашивается
graceful disconnect. Timeout disconnect помечается отдельно и даёт exit code 1.

ENet host обслуживает 4 channels. Nonce/generation проверяются для каждого
соединения; sequence counters отдельные для `(ordering,reliability)`. Mode3
требует `last+1`; modes1/4 и текущая stronger-ordered реализация mode2 принимают
только `>last`; mode0 не отвергает допустимый out-of-order. Исходящие MTA packets
пока только reliable-ordered mode3/channel0. Graceful disconnect использует
ENet data=1; сервер отдельно доказывает настоящий `onPlayerQuit`.

## Реальная последовательность join

1. ENet connect → MTAX hello → welcome с server nonce/generation.
2. `PACKET_ID_MOD_NAME`: проверить bitstream version и `deathmatch`.
3. `PACKET_ID_PLAYER_JOINDATA`: версии, sortable version, update bit, game version,
   fixed nickname, password, removed community bytes по `CPlayerJoinDataPacket`.
4. `PACKET_ID_SERVER_JOIN_COMPLETE`: два server version strings → RPC
   `PLAYER_INGAME_NOTICE` из authoritative enum.
5. `PACKET_ID_SERVER_JOINEDGAME`: настоящий own-player ID и root ID, client checks,
   voice, HTTP и server name → RPC `INITIAL_DATA_STREAM`.
6. Client log `initial_data_requested` означает только отправку запроса. Actual
   server `Player.SetJoined` и `onPlayerJoin` подтверждаются независимо witness.

У каждого join phase свой timeout. Неожиданные stage/session/channel, malformed
payloads и серверный disconnect дают `fatal_error` и exit code 1. Остальные
пакеты явно логируются как `packet_not_implemented`; этот клиент не скачивает
ресурсы, не исполняет client.lua и не подтверждает их запуск.

## Witness и entity references

Ожидается серверный ресурс `experiments/headless-witness`, включённый консолью
`witness start`. Client получает actual `PACKET_ID_LUA_EVENT` с именем
`headlesswitness:probe`, source root и тремя аргументами `(token,correlation,
vehicleElement)`. Декодируются реальные Lua type bits и ElementID. Ответ
`headlesswitness:ack` отправляется через настоящий Lua-event packet с source
**своего player ID**, а не с source из входящего события.

`witness_ack_sent` не означает серверное принятие: нужен независимый server
`ack` с тем же token/correlation. Correlation не подменяет internal ElementID.
Client может вернуть decoded element reference, даже когда его prefix ещё не
наблюдался; поле `entity_prefix_model=not_observed` честно показывает эту границу.

`ENTITY_ADD` decoder ограничен первым common prefix и vehicle position/model.
Остальное тело и последующие entities не пропускаются наугад: логируется
`remaining_bits` и ограниченный scope. Неподдержанный custom Lua type даёт
`entity_prefix_unsupported`. Это не создание native vehicle и не полный registry.
`ENTITY_REMOVE` декодирует последовательность actual ElementIDs и логирует их.
Другие state/sync packets пока не реализованы.

JSONL stdout содержит `event`, числовой `elapsed_ms`, nickname/state и прочие
поля-строки. Сохранять stdout/stderr, process exit, executable hash, build ID,
server/module hashes и server witness log. Успешный выход CLI сам по себе не
закрывает H1, а H1 не закрывает GTA gameplay, Android, touch или общий физический мир.

## Authoritative source paths

- `Server/mods/deathmatch/logic/packets/CPlayerJoinDataPacket.cpp`.
- `Server/mods/deathmatch/logic/packets/CPlayerConnectCompletePacket.cpp`.
- `Server/mods/deathmatch/logic/packets/CPlayerJoinCompletePacket.cpp`.
- `Server/mods/deathmatch/logic/CRPCFunctions.h/.cpp` и `CGame::InitialDataStream`.
- `Server/mods/deathmatch/logic/packets/CLuaEventPacket.cpp`.
- `Server/mods/deathmatch/logic/lua/CLuaArgument.cpp`, `CLuaArguments.cpp`.
- `Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp`, `CEntityRemovePacket.cpp`.
- `Shared/sdk/net/SyncStructures.h`: SLuaTypeSync, SPositionSync, SRotationDegreesSync.
