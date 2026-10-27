# Android NDK cross-build существующего сетевого harness

Цель — собрать **те же** `net_probe.cpp`, `wire_tests.cpp` и `wire.hpp` для Android. Отдельного протокола или другого codec здесь нет. MTA network modules, GTA и общий игровой мир этим экспериментом не проверяются.

## Проверено 2026-10-28

- Реально скомпилированы `net_probe` и `wire_tests` для `arm64-v8a` и `x86_64`, API 26, Release, NDK r28c/Clang 19.0.1, по одному compile worker.
- LLVM readelf подтвердил обе 64-битные архитектуры, Android interpreter `/system/bin/linker64` и PIE. В CMake cache нет Python3, Android runtime CTest не регистрировался. Использован native Ninja из SDK.
- Cache ENet проверен по 38 hashes. Копия с изменённым `LICENSE` отвергнута до configure; прежний искусственный successful result заменён terminal `failed`.
- Определения трёх host tests совпадают с базой; это также проверено независимым review. Host runtime на этой ветке повторно не запускался.
- C++ sources и общий wire header не изменены. Android ELF не исполнялись, packets на Android не отправлялись, emulator не запускался.

Снимок hashes и ограничений: `verification/cross-build-2026-10-28.json`. **Это завершение cross-build части A1; весь gate A1 остаётся `not_run` до настоящего Android↔host обмена.**

## Запуск

Нужны существующие CMake 3.24+, Ninja, NDK r28c и локальный cached source ENet. Скрипт не устанавливает инструменты и не скачивает зависимости. На Windows:

```powershell
& './experiments/mobile-net/android/Build-Android.ps1' `
    -EnetSource 'D:/projects/astana/work/mtasa-mobile-net-worktree/experiments/mobile-net/build/_deps/enet-src' `
    -NdkRoot 'C:/Android/ndk/28.2.13676358' `
    -Abis 'arm64-v8a'
```

Для отдельной второй сборки передай `-Abis 'x86_64'`. По умолчанию перечислены обе ABI; **при нехватке RAM запускай их по одной**, после завершения baseline MTA. Компиляция всегда `--parallel 1`. Параметры: `OutputRoot`, `ApiLevel` (default 26), `Cmake`, `Ninja`.

Обе программы собираются как Android ELF с `c++_static`; Android dynamic linker остаётся `/system/bin/linker64`. Статическая C++ runtime не превращает executable в полностью статический ELF.

`FETCHCONTENT_SOURCE_DIR_ENET` и `FETCHCONTENT_FULLY_DISCONNECTED=ON` обеспечивают использование локального ENet. Override source path обходит повторную проверку archive hash самим FetchContent, поэтому helper до configure сравнивает 38 файлов выбранного cache с hashes в `verification/dependency.json`. Эти hashes получены из исходного archive, SHA256 которого соответствует pin в основном CMakeLists. Изменённый cache отклоняется, fixed commit ему не приписывается.

## Разделение host и Android

В CMake добавлен только `if(NOT ANDROID)` вокруг прежних трёх host tests и поиска Python. На Android обе программы компилируются, Python не требуется и runtime CTest не регистрируется. На host имена/команды трёх тестов сохранены.

`Build-Android.ps1` пишет configure/build/readelf logs и `compile_commands.json` под `build-android/<ABI>`, общий `build-evidence.json` — в `OutputRoot`; каталог игнорируется существующим `/build*/`. В evidence есть hashes исходников и бинарников, source HEAD, ABI, API и отдельный `runtime=not_run`. В начале запуск инвалидирует прежний успешный результат значением `status=running`; исключение сохраняет `status=failed`, error и уже завершённые ABI records. Смотри также exit code и лог runner.

## Границы проверки

Успех cross-build и ELF inspection означает только переносимость сборки этих sources. Android executable не запускался, wire golden vectors на Android не исполнялись, host↔Android packets не передавались. Готовность A1 runtime и A3 gameplay не объявляется.

Существующий `net_probe` использует loopback `127.0.0.1` и не принимает отдельный host-address parameter. Для будущего Android↔host smoke потребуется отдельно согласованное расширение этого входа и подходящий endpoint (например, emulator host alias либо LAN address). Здесь `net_probe.cpp` не меняется. Boot blocker Android lab этой сборкой не снимается.

Перед распространением native binaries включай MIT license ENet из проверенного source tree. GTA libraries и игровые данные не входят в эксперимент.
