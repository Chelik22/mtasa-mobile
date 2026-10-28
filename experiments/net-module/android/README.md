# Android native H1 protocol client

Отдельный CMake project собирает **те же** `headless/main.cpp`, `MtaBitStream.cpp`, `ModuleRuntime.cpp`, `headless/codec_tests.cpp` и `protocol_tests.cpp`, что использует настоящий Windows H1. Protocol/codec не копируются. Это подготовка Android→host MTAX/ENet и MTA packet interaction; GTA, client Lua VM и native world сюда не входят.

Профиль фиксирован: Android x86_64, API26, `c++_static`, установленный NDK r28c. `ModuleRuntime` выбирает существующую pthread-ветку. Нет Windows DLL/server targets, host Python discovery, CTest runtime или downloader. Cached ENet сверяется с единственным repository manifest из `experiments/mobile-net/android/verification/dependency.json`: все38 hashes pinned archive2662c0de09e36f2a2030ccc2c528a3e4c9e8138a. Отсутствующий/изменённый cache — configure failure, не повод скачать замену.

## Configure отдельно от cold build

```powershell
& './experiments/net-module/android/Configure-Android.ps1'
```

Defaults соответствуют уже установленным local tools/cache; для другого checkout передай `EnetSource`, `NdkRoot`, `Cmake`, `Ninja`, `OutputRoot`. Configure сохраняет собственный CMake PID/start, аргументы, source SHA/hashes и live stdout/stderr в ignored `android/build/x86_64-api26`. При активном recorded PID повторный запуск отвергается. Tiny NDK/compiler/function probes во время configure допустимы; три application targets эта команда не собирает и не запускает.

Проверка generated constants на host: `python experiments/net-module/headless/check_source_constants.py --check`. Сам Android CMake project не требует Python; константы используются из того же checkout.

**Cold compile пока не разрешён:** G1 MSBuild имеет приоритет по RAM. После отдельного согласования build выполняется одной командой с одним worker:

```powershell
cmake --build experiments/net-module/android/build/x86_64-api26 --parallel 1 --target headless_client headless_codec_tests protocol_tests
```

После реальной сборки зафиксировать hashes и NDK llvm-readelf header/program headers/dynamic dependencies. До этого target build, ELF verification и Android runtime — not_run. Configure success не доказывает source compilation или runtime.

## Будущий actual ADB сценарий

Следующие команды — план, **не выполнены**. Требуются reviewed binaries, owned Android native shell и отдельно запущенный source-built H1 server с experimental module/исходным Lua witness. Windows server сохраняет bind `127.0.0.1`; firewall/Windows settings и внешние interfaces не меняются. Для Android Emulator адрес `10.0.2.2` обращается к host loopback; это не адрес для физического телефона.

```powershell
$adb = 'C:/platform-tools/adb.exe'
$serial = 'emulator-5580'
& $adb -s $serial shell mkdir -p /data/local/tmp/mta-h1
& $adb -s $serial push ./experiments/net-module/android/build/x86_64-api26/headless_client /data/local/tmp/mta-h1/headless_client
& $adb -s $serial push ./experiments/net-module/android/build/x86_64-api26/headless_codec_tests /data/local/tmp/mta-h1/headless_codec_tests
& $adb -s $serial push ./experiments/net-module/android/build/x86_64-api26/protocol_tests /data/local/tmp/mta-h1/protocol_tests
& $adb -s $serial shell chmod 700 /data/local/tmp/mta-h1/headless_client /data/local/tmp/mta-h1/headless_codec_tests /data/local/tmp/mta-h1/protocol_tests
& $adb -s $serial shell /data/local/tmp/mta-h1/headless_codec_tests
& $adb -s $serial shell /data/local/tmp/mta-h1/protocol_tests
```

До старта клиента передать новый пустой command file (не переиспользовать и не обнулять работающий). Подставить **порт из конкретного изолированного H1 runtime receipt**:

```text
adb -s emulator-5580 shell /data/local/tmp/mta-h1/headless_client --host 10.0.2.2 --port <H1_UDP_PORT> --identity CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC --nickname H1_Android --duration 180 --commands /data/local/tmp/mta-h1/android.commands
```

Процесс CLI должен иметь собственные PID/log/exit и наблюдаться до terminal. Второй endpoint — уже проверенный Windows H1 client с другим identity/nickname. В командный файл через ADB добавляются полные строки `status`, `disconnect`, `connect`, `reconnect`, `quit`; append-only/64KiB contract прежнего клиента сохраняется. Клиент автоматически использует настоящий MTA join/packet flow и Lua event codec. Native CLI может работать до `boot_completed`, но это не закрывает Android framework/game gate.

Для принятия результата нужны actual source-server player/ElementID receipts, прежний Lua witness vehicle assertions/roundtrips, reconnect и cleanup обоих endpoints; отдельно stdout/exit codec binaries. Один Android client, codec PASS или transport handshake не заменяют этот сценарий. Полный CLI orchestration adapter к private H1 runner ещё предстоит подготовить; существующий Windows runner нельзя объявлять совместимым с ADB без испытания.

## Текущий статус

Source/constants check и PowerShell parser выполнены. Actual configure PID47300 завершился exit0, Clang19.0.1/NDKr28c; проверены38 ENet hashes и точный source file set. Ninja graph содержит14 translation units и три заданных application targets. Их executable files отсутствуют: cold build, ELF verification и все Android/host executions not_run. [Configure snapshot](verification/configure-2026-10-28.json) содержит actual source/hash/PID/log evidence; никаких AVD/APK/GTA binaries в Git нет.

Изменения этого subproject обратимы удалением отдельного Android entry point; host CMake и application sources не меняются.
