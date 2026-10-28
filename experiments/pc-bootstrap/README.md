# Bootstrap собственной сети до Core

`loader-proxy` выбирает тот же фиксированный `netc_experimental` stem, что и
Core, по `MTA_EXPERIMENTAL_CLIENT_NET_MODULE`. Пустое, неизвестное, слишком
длинное значение или путь отклоняется до LoadLibrary. Release/Debug filenames
согласованы. Без переменной сохранён штатный `netc` путь.

Штатный proxy требует и вызывает private `CheckService(7)` до загрузки Core.
Наш transport этой услуги не реализует и не экспортирует. В явной experimental
ветке proxy вместо неё проверяет настоящий public `CheckCompatibility` с
`MTA_DM_CLIENT_NET_MODULE_VERSION`; Core повторяет штатную ABI-проверку при
создании своего интерфейса. Отсутствующий export или несовместимая версия
останавливают bootstrap. Это не реализация official AC/post-create service.

Standalone CMake собирает две версии маленького contract test и actual
`Client/loader-proxy/main.cpp` как object-only target. Тесты проверяют настоящий
Win32 environment, dispatch ошибок/stock/owned и LoadLibrary ранее проверенной
owned DLL. Они не запускают proxy, Core или GTA и не доказывают G4.

```powershell
cmake -S experiments/pc-bootstrap -B <fresh ignored build> -G "Visual Studio 18 2026" -A Win32 -DMTA_OWNED_NET_DLL=<verified netc_experimental.dll>
cmake --build <fresh ignored build> --config Release --parallel 1
ctest --test-dir <fresh ignored build> -C Release --output-on-failure
```

Actual Win32 proxy object compile и два native contract tests прошли: build0,
CTest2/2, LoadLibrary настоящей owned DLL и отказ неправильного ABI.
[Receipt](verification/windows-x86/receipt.json). Первые две компиляции выявили
SDK-запреты tick API в lab helper; исправлено через standard chrono.
Полный Core build, native userpaths isolation и игровой запуск — отдельные
предпосылки, которые этот тест не заменяет.
