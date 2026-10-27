# Android backend: проверенные границы и первые шаги

Дата исследования: 2026-10-28. Это чтение опубликованных исходников, без запуска
GTA, APK и поставляемых библиотек. Код и бинарники мобильного клиента сюда не
копировались. Совместная игра и лицензии всех зависимостей не проверены.

## Зафиксированные версии

- MTA: `a84e0c77c66f596b0c72def1c857253568208a2b`.
- `kuzia15/SAMP-Mobile`, `GTA-2.11`:
  `0a805d4c3d1adbc0baccd02125901f9987cb1ab1`.

## Реальная точка адаптации

MTA `CPools::AddVehicle(CClientVehicle*, uint16_t, uint8_t, uint8_t)` создаёт
игровое воплощение клиентского элемента. Верхний `CClientVehicle` вызывает этот
метод, затем устанавливает stored pointer, матрицу, скорости и видимость.
Сохранение серверных элементов и верхней модели MTA позволяет направлять их к
другой реализации игровых вызовов.

В мобильной базе `CGame::NewVehicle` вызывает конструктор `CVehicle`.
Конструктор загружает модель через `CStreaming::TryLoadModel`, вызывает
`ScriptCommand(&create_car, ...)`, получает объект `GamePool_Vehicle_GetAt`,
меняет матрицу и настройки. Для части машин присутствует смещение Z на 0.25:
семантику координат необходимо согласовать, а не копировать вызов буквально.
`localplayer.cpp` получает позицию и скорость из персонажа перед заполнением
SA-MP структуры синхронизации. Саму SA-MP синхронизацию сохранять необязательно.

Сопоставления для узкой реализации:

| MTA | Android-кандидат | Что ещё проверить |
|---|---|---|
| `CPools::AddVehicle` | `CGame::NewVehicle`, `CVehicle` | Модель, variants, ошибки загрузки, lifetime |
| `CGame::GetStreaming`, `GetModelInfo` | `CStreaming::TryLoadModel`, `RequestModel`, `LoadRequestedModels` | Владение reference и освобождение модели |
| `SetPosition`, `SetMatrix`, `GetMoveSpeed` | Игровые `SetPosn`, `SetMatrix`, `GetMatrix`, `GetMoveSpeed` | Единицы, координатная система, интерполяция |
| `CPools::AddPed` | `CGame::NewPlayer`, `CPlayerPed` | Local/remote ped, pool и input ownership |
| `RemoveVehicle`, `RemovePed` | Отдельный backend lifecycle | Нельзя смешивать SA-MP и MTA ownership |

Источники:

- [MTA CPools](https://github.com/multitheftauto/mtasa-blue/blob/a84e0c77c66f596b0c72def1c857253568208a2b/Client/sdk/game/CPools.h).
- [MTA CGame](https://github.com/multitheftauto/mtasa-blue/blob/a84e0c77c66f596b0c72def1c857253568208a2b/Client/sdk/game/CGame.h).
- [MTA CClientVehicle](https://github.com/multitheftauto/mtasa-blue/blob/a84e0c77c66f596b0c72def1c857253568208a2b/Client/mods/deathmatch/logic/CClientVehicle.cpp).
- [Mobile game.cpp](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/app/src/main/cpp/samp/game/game.cpp).
- [Mobile vehicle.cpp](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/app/src/main/cpp/samp/game/vehicle.cpp).
- [Mobile localplayer.cpp](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/app/src/main/cpp/samp/net/localplayer.cpp).

## Граница пока негерметична

- `Client/mods/deathmatch/StdInc.h` включает `windows.h`.
- `CClientVehicle.cpp` включает конкретные `game_sa/CVehicleSA.h` и
  `game_sa/CVehicleAudioSettingsEntrySA.h`.
- `CPools` использует `SClientEntity<CVehicleSA>` и `DWORD*` в интерфейсе.
- В `CClientVehicle.cpp` есть комментарий о pointer truncation `DWORD*` в
  `unsigned long`. Это необходимость проверки ARM64, не доказанный crash.
- Mobile `vehicle.cpp` сам включает SA-MP `netgame.h`, использует `pNetGame`.
  Извлечение игрового backend требует разрыва этих зависимостей.

Поэтому мобильный subclass `CGame` сам по себе не сделает верхний клиент
переносимым. Сначала убрать конкретные PC-типы из выбранной вертикальной части;
не начинать широкое переименование всего SDK.

## ABI и game binary

Фактический `app/build.gradle.kts`: только `arm64-v8a`, `minSdk=28`,
compile/target SDK 36, NDK `26.2.11394342`, Java 17. README обещает Android 8.0+,
что противоречит minSdk 28: выбранная Gradle-конфигурация требует Android 9+.
Наличие ARM32 ветки в CMake не доказывает поддержку ARM32 выбранным APK.

CMake линкует EGL/GLESv3, Opus, ShadowHook и поставляемые GlossHook/BASS binaries.
В `jniLibs/arm64-v8a` находится `libGame.so`; `main.cpp` ищет именно эту
библиотеку, затем использует version-specific offsets. Произвольный легальный
APK GTA нельзя объявить совместимым без проверки fingerprint и адресов.
Флаг linker alignment 16 KiB не доказывает совместимость всех bundled `.so`.

x86_64 AVD подходит для чистого smoke APK, но не доказывает запуск ARM64-only
game library. Перед игровой стадией нужны подтверждённый native bridge либо
ARM64 device/emulator. ABI smoke и game smoke считать разными результатами.

- [Gradle](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/app/build.gradle.kts).
- [CMake](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/app/src/main/cpp/samp/CMakeLists.txt).
- [main.cpp](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/app/src/main/cpp/samp/main.cpp).

## Происхождение и лицензии

README выбранной mobile ветки содержит условие `educational and research
purposes only`. В recursive tree не найден файл с `license` в пути. Это не
подтверждает разрешение на коммерческий reuse и не заменяет пофайловый аудит.
Не подтверждены права на распространение `libGame.so`, остальных binaries и
совместимость условий с GPL MTA. Публичность репозитория не решает эти вопросы.
До проверки происхождения использовать source references и чистые интерфейсы;
не переносить bundled game binaries в fork.

- [Mobile README](https://github.com/kuzia15/SAMP-Mobile/blob/0a805d4c3d1adbc0baccd02125901f9987cb1ab1/README.md).
- [Лицензия MTA](https://github.com/multitheftauto/mtasa-blue/blob/a84e0c77c66f596b0c72def1c857253568208a2b/LICENSE).

## Очередь заданий для Android-агента

1. **A0 — инструментальная проверка.** Чистый native smoke APK без GTA libs:
   ARM64 compile, install, JNI, CLI trigger, logcat. Запуск x86_64 варианта
   засчитывается только за x86_64. Результат не является игровым портом.
2. **A1 — узкий контракт.** Platform-neutral entity handle, transform и
   load/create/read/write/destroy. Сетевой ID не pointer и не pool handle.
   Собрать PC и ARM64; в выбранной части не должно быть Win32/game_sa типов.
3. **A2 — локальный игровой smoke.** Только после проверки game fingerprint и
   происхождения библиотек: штатные model 411 и skin. Сто циклов create/read/
   move/destroy, счётчики handles и ошибок. Ошибка загрузки модели конечна,
   освобождение предсказуемо. Не подключать CEF, карту и кастомные эффекты.
4. **A3 — identity.** Отображение `server ElementID -> client entity -> native
   handle`. Stream-out удаляет местное воплощение, окончательное уничтожение
   удаляет mapping. Проверить позднее подключение и повторное создание.
5. **A4 — fixture.** Запустить соседний Lua-ресурс на общем сервере PC/Android:
   одно создание, одинаковый correlation, ACK обеих платформ, cleanup/restart.
   Независимо подтвердить наличие одного серверного элемента. Correlation
   fixture не подменяет внутренний ElementID.
6. **A5 — игра.** Remote player, затем общий водитель/пассажир и передача
   управления. Сохранить MTA authority rules; разные клиенты не назначают
   владельца независимо. Только затем переносить по одному реальному RP
   ресурсу с сохранённой серверной частью.

Каждый шаг имеет статус `not_run/passed/failed` и сырые логи. Echo-протокол,
запуск пустого AVD и синтаксическая проверка Lua не закрывают A2–A5.

## Обязательное R1/A3: мобильное управление и lifecycle

Android — отдельная платформа ввода. Передавать в верхнюю игровую логику
семантические действия `move`, `look`, `aim`, `fire`, `accelerate`, `brake`,
`steer`, `enterVehicle`, а не синтезировать цепочку PC keycodes. Слой ввода
отделяет touch-pointer IDs, мышь/клавиатуру и optional controller от этих
действий. Аналоговые оси имеют явные диапазоны и правила сброса; сетевой слой
получает игровое состояние через обычную модель MTA, а не raw touch events.
Точный набор действий фиксируется выбранной игровой вертикалью.

Для R1/A3 нужен отдельный протокол проверки на реальном touch device:

- Одновременно движение и camera look; aim+fire вместе с движением. Отдельные
  pointer IDs сохраняются при добавлении и отпускании третьего пальца.
- В машине одновременно steer+accelerate/brake и камера; вход/выход меняет
  контекст управления без оставшихся действий предыдущего контекста.
- `ACTION_CANCEL`, потеря фокуса, закрытие overlay и уход в background
  сбрасывают удерживаемые действия. После возврата нет stuck fire/throttle.
- Android Back закрывает текущий overlay/IME по оговорённому приоритету;
  ввод в чат не управляет персонажем. Появление и скрытие IME не ломает viewport.
- Safe area/cutouts и минимум два размера/плотности экрана: элементы доступны,
  масштаб UI не меняет чувствительность камеры и не перекрывает обязательные
  действия. Это критерии будущей UI-реализации, а не готовый дизайн.
- Pause/resume проверены в on-foot и vehicle контекстах. Сервер получает
  определённое состояние соединения; после reconnect нет повторного local ped,
  fixture vehicle и старых callbacks. Не маскировать потерю соединения.
- Controller support optional и не блокирует touch acceptance; при его
  подключении/отключении семантические действия не остаются зажатыми.

Эмуляторные `adb shell input tap` и один pointer не закрывают multitouch gate.
Нужны независимые запись действий/состояний и визуальное подтверждение.
