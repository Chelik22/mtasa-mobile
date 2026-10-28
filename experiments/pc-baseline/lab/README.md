# Opt-in PC lab paths

`MTA_EXPERIMENTAL_LAB_ROOT` выбирает отдельный существующий локальный root.
Это исходный Windows/MTA-client режим для будущего собственного клиента; он
ещё не доказывает изоляцию нативной GTA или готовность к игровому запуску.

Все девять каталогов должны существовать до запуска: `app`, `gta`, `data`,
`common`, `documents`, `local-appdata`, `temp`, `settings`, `logs`. Root должен
быть абсолютным; traversal, UNC и reparse points в его пути/дочерних каталогах
отвергаются. Отсутствующая переменная сохраняет штатный путь. Явная пустая,
невалидная или неполная конфигурация вызывает отказ до записи; fallback к
установленной игре не используется. Контекст захватывается при первом обращении
каждого модуля и после этого не меняется вместе с environment.

`SharedUtil.File.hpp` направляет MTA data/common/personal/local/temp в этот root,
`GetMTASABaseDir` и loader discovery используют только `app`/`gta`. MTA product
registry settings хранятся в `settings/registry.bin`: настоящие case-insensitive
операции read/write/delete, межпроцессный file lock, bounded versioned records и
atomic publish после flush. Данные ограничены 8MiB/4096 entries/1MiB value;
повреждение не превращается в пустой store. Файл не использует Windows INI API
или глобальную registry virtualization. Запросы системных registry keys остаются
обычными read-only OS queries. Вне opt-in штатный backend не меняется.

Lab loader не записывает WER/AppCompat/IFEO настройки, не устанавливает service,
не повышает права и не переключается на updater/ShellExecute fallback. Он не
передаёт URI уже работающему MTA и не завершает найденные по имени/PID процессы.
Очистка собственного `CreateProcess` child сохраняет retained process handle.
Если необходимы системные repair или конфликтует другая GTA/MTA, запуск требует
отдельного решения; этот режим не сообщает об успешном выполнении repair.

Native GTA может обращаться к Documents через собственные API, минуя
`SharedUtil`. Этот путь пока не исследован до достаточного уровня. **GUI/GTA не
запускались и не разрешены этим изменением.** Следующий этап требует REA/source
аудита этого поведения и независимого review, затем согласованного incremental
client build. Установленные MTA/GTA и baseline `Bin` не менялись.

## Изолированный тест helper

```powershell
cmake -S experiments/pc-baseline/lab -B experiments/pc-baseline/lab/build-win32 -G 'Visual Studio 18 2026' -A Win32
cmake --build experiments/pc-baseline/lab/build-win32 --config Release --parallel 1 --target lab_paths_probe lab_server_routes -- /m:1 /nr:false /p:MultiProcessorCompilation=false
ctest --test-dir experiments/pc-baseline/lab/build-win32 -C Release --output-on-failure --parallel 1 --no-tests=error
```

Actual Win32 component build и CTest прошли: один CTest запускает пять Python
filesystem contract tests через настоящий отдельный probe executable. Проверены
absent/invalid root без Registry API, freeze/closed child namespace, independent
process persistence, Windows key casing, отказ delete parent с subkey,
oversize/corrupt store без перезаписи и два независимых writer процесса без
потери updates. Стенд не загружает MTA, DLL или GTA. После review дополнительно
скомпилирован настоящий `SharedUtil.File.hpp` в Windows server TU без
`MTA_CLIENT`: он ловит незащищённые ссылки на client-only namespace. Первый
такой attempt003 остановился на отсутствующем vendor include path; после
добавления существующего repository include attempt004 прошёл compile и снова
все пять filesystem tests. SDK **client** routing и полный client rebuild пока
не компилировались. Первоначальный snapshot находится в
`verification/windows-x86-2026-10-28.json`; raw receipts/логи сохраняются вне Git.
Server-mode regression зафиксирован отдельно в
`verification/windows-server-routes-2026-10-28.json`.

Предыдущий attempt001 был deferred при 1890MiB до native child. Attempt002 прошёл
с отдельно разрешённым floor1024MiB для одного standalone TU, `Normal`, `/MP1`,
одним build worker. Этот floor не распространяется на engine/NDK сборки.

## Подготовленный native user-directory hook

`Shared/sdk/ExperimentalNativeUserPaths.h` предназначен для раннего
`loader-proxy::OnGameLaunch`, до загрузки net/Core и возврата в native startup.
Anchor пока не подключён. Stock path без opt-in не меняется. Поддерживается
только x86 canonical SHA
`f63fa623d14e170d0ae9e7032186669cf3a177793be21c4824043f0279bd506a`
либо тот же образ после нормализации шести штатных loader patches. Фиксированные
offsets проверены по pinned `GameExecutablePatcher.cpp`; неизвестная правка кода
не маскируется и должна провалить полный SHA.

Штатный launch EXE: `root/data/GTA San Andreas/gta_sa.exe`; assets: `root/gta`.
Helper проверяет image base, исходные16bytes routine0x744FB0 и ещё пустые три
256-byte native buffers до native writes. ACP conversion обязана точно
round-trip; каждый путь вместе с NUL<=256bytes. Directory reparse проверяется
до публикации; hook поддерживает base/Gallery/User Tracks и возвращает настоящий
base buffer. Detour меняет только память процесса,7bytes целых инструкций,
сохраняет protection и flushes instruction cache. При любой ошибке установки
owned child должен завершиться до native continuation. Callback сам завершает
его при последующем нарушении private-directory policy.

Standalone target `native_paths_probe` —1TU,Win32,/MP1,m1; CTest regex
`^native_user_paths_`. CMake `-DLAB_CANONICAL_EXE=<private lab gta_sa.exe>`
включает проверку реальных canonical bytes и normalizer; без этого параметра
такого доказательства нет. Файл игры читается только как данные. Остальные
tests проверяют private filesystem публикацию и actual detour tiny owned
function в VirtualAlloc memory; это не выполнение GTA. Native registry helpers
вне directory routine ещё требуют аудита: один hook не закрывает полную
изоляцию процесса и G1/gameplay.
