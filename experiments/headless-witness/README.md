# Server-only Lua witness: B0 и будущий H1

Ресурс исполняется только настоящим MTA server. В нём нет `client.lua`, fake
player, native game libraries, downloaded assets и production конфигурации.
По умолчанию H1 выключен. Наличие исходника и Lua syntax PASS не доказывают
исполнение сервера, загрузку экспериментального сетевого модуля или общий мир.

## Изолированный запуск

1. Собрать настоящий сервер из зафиксированного commit; записать source/build
   IDs, toolchain, архитектуру и hash фактически загруженного сетевого модуля.
2. Скопировать эту папку в `resources/headless-witness` отдельного test runtime.
   Не менять ACL/БД/production ресурсы. Ограничить listener loopback средствами
   конфигурации сервера до подключения других машин.
3. В консоли сервера: `start headless-witness`, `witness status`.
   Все команды этого ресурса разрешены только консоли; даже ACL admin-player
   отклоняется. Resource не выдаёт ACL права и не запускает shell.
4. Сохранить полный server log. Строки после `[headlesswitness] ` содержат
   результат `toJSON(record, true)`. MTA может оборачивать объект в JSON array;
   parser должен учитывать фактический вывод, а не полагаться на порядок ключей.

## B0: сервер и source Lua без игроков

Выполнить `witness reset`, убедиться, что реальных players 0, затем `witnessb0`.
Ожидается `b0_result` с `passed=true`, create/model/position/destroy assertions,
таблицей `getVersion()` и players=0. Это реальные вызовы серверного Lua API
создания/чтения/удаления vehicle; GTA и клиентская графика для B0 не требуются.
Если runtime не запущен, B0 остаётся `not_run`. B0 не закрывает H1.

## H1: два настоящих headless protocol clients

1. `witness start` включает наблюдение; до двух actual MTA player elements
   fixture vehicle не создаётся. `onPlayerJoin` наблюдается от самого сервера.
   Если два player уже есть при старте, это отражается в status; не называть их
   новыми `onPlayerJoin` событиями.
2. Когда players >=2, ресурс создаёт ровно один server vehicle model 411.
   Повторный `witness start` не дублирует существующий vehicle.
3. Каждому actual player сервер отправляет настоящий `triggerClientEvent`:
   event `headlesswitness:probe`, source `root`, аргументы `(token, correlation,
   vehicleElement)`. `dispatched=true` означает успешный вызов API, не доставку.
4. Headless client должен получить MTA resource/entity/event protocol, декодировать
   реальную ссылку на тот же server element и отправить actual Lua event
   `headlesswitness:ack` с source **своего player element** и теми же тремя
   аргументами. Этот receiver не реализован данным resource. Echo ENet сообщения
   не являются этим действием и не удовлетворяют H1.
5. Сервер принимает ACK только при actual `client == source`, ожидаемом токене,
   correlation, той же ссылке vehicle и сроке менее 10 секунд. После одного
   приёма токен удаляется; повтор, другой source/element/token — `ack_rejected`.
   Без ответа — `probe_expired`. Это fixture correlation, не authentication.
6. Отключить одного клиента: требуется настоящий `player_quit`; существующая
   машина сохраняется. При reconnect новый connection label, настоящий join и
   новый токен, тот же correlation текущей машины. Это не восстановление MTA
   serial/account: labels только показывают разные соединения текущего run.
7. `witness status`: сохранить фактические joins/quits/connections_seen и
   индивидуальный ack каждого текущего player; aggregate acks сам по себе не
   доказывает, что ответили оба клиента.
8. `witness reset`: отключить H1, уничтожить vehicle, инвалидировать tokens.
   Оба клиента должны обработать реальное удаление элемента. Счётчики текущего
   run сохраняются для аудита. После нового start создаётся новый correlation,
   если players по-прежнему >=2. Stop/restart resource начинает новый run.

Correlation — серверная диагностическая строка, **не internal ElementID**, не
GTA pool handle и не указатель. H1 дополнительно требует независимых клиентских
логов actual network element references, version/build identity, результатов
удаления и disconnect/reconnect. Одних witness log или поддельных player tables
недостаточно. H1 не доказывает GTA gameplay, движение, Android или multitouch.

После expiration можно выполнить `witness start` повторно: без дублирования
vehicle будут выданы новые tokens участникам без текущего ACK. Reset оставляет
старые токены непригодными. Для сравнения PC и Android позднее использовать
полноценный world fixture и отдельные input/lifecycle gates.

## Проверка пакета

`python experiments/headless-witness/validate.py` из корня репозитория проверяет
XML и единственный source-only server script. Lua 5.1 `luac -p server.lua`
проверяет синтаксис при наличии CLI. Любой mock сам по себе не меняет статус
B0/H1: runtime evidence создаётся только настоящим source-built MTA server.
