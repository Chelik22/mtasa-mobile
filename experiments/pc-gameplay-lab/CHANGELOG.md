# Changelog

## Unreleased

- Добавлен отдельный стандартный server Lua spawn для игрового fixture;
  серверный RP и shared-world fixture не меняются.
- Зафиксированы реальные cache path и необходимость отдельного approved HTTP
  manifest для G4; component service DLL не выдаётся за gameplay configuration.
- Fresh server preflight001 исполнил bootstrap Lua, но выявил отсутствующую
  `ValidateHttpCacheFileName` реализацию server net module; gameplay fixture
  отклонён. Failed logs сохранены; игровой G4 не объявлен пройденным.
- Fixture различает MTA console element и player. Подготовка нового server net
  module требует явного SHA256 и создаёт отдельный пустой resource-cache.
