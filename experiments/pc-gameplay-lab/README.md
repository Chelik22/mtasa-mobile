# Первый настоящий игровой клиент

`bootstrap/` — отдельный server-only ресурс обычного MTA. Он вызывает штатные
`spawnPlayer`, `fadeCamera`, `setCameraTarget` рядом с машиной из
`mobile-world-fixture`. Серверные RP-ресурсы не изменяются. Общий fixture
отличает настоящий MTA console element от player при именовании, chat и ACL.

Для G4 нужны оба ресурса: `mobilelab-spawn` (эта папка bootstrap под таким
именем в server resources) и `mobile-world-fixture`. После actual client Lua
`http_asset_read` / `resource_start` консольная команда
`mobilefixture create` должна дать настоящую машину и проверенный probe ACK.
Наличие server spawn log отдельно не доказывает отрисовку или client Lua.

Обычный путь cache из реального `CClientGame::SetFileCacheRoot` при пустых lab
settings — `<lab>/app/mods/deathmatch`. `CResource` добавляет
`resources/mobile-world-fixture/http-witness.txt`. Поэтому trusted manifest
должен использовать `--destination-prefix resources/mobile-world-fixture`, а
`MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT` — корень mod, не папку ресурса.
Фиксированный URL/HTTP port должны совпадать с новым isolated server config;
DLL из configured service test имеет другой manifest и автоматически сюда
не подходит. HTTP/server/cache/client Lua должны проверяться в actual G4.

Подготовка свежего сервера без переноса БД и логов H1:

```powershell
python experiments/pc-gameplay-lab/prepare_server.py --h1-plan <verified h1-plan-001.json> --destination <fresh lab server directory>
```

Native inputs берутся только из восьми явных hash-verified H1 entries. Новые
config/ACL привязывают сервер к loopback; команду `mobilefixture` получает только
Console. Для запуска нужен `MTA_EXPERIMENTAL_NET_LIBRARY=net_experimental.dll`;
штатная CLI сервера — `-t -u -D <runtime> --config mtaserver.conf --ip 127.0.0.1
--port 23803 --httpport 23805 --novoice`. Сначала убедиться, что порты свободны.

Подготовка ресурса не означает, что GTA, spawn path с игроком или G4 уже
исполнялись. Production server/RP/DB в этот сценарий не входят.
