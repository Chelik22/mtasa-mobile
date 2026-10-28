# Android A2: статическая карта engine backend

Вердикт: у исследованной ARM64 библиотеки есть именованные кандидаты для узкой вертикали «игрок + одна машина + ввод». Это **не реализация backend и не доказательство callable ABI**. A2 static inventory выполнен; загрузка игры, engine calls, позиция, машина, touch и совместный мир в runtime — `not_run`.

## Граница версии и происхождения

Исследуется только локальный `GTA-SA-v2-11-311-mod.apk`, SHA256 `348750d29d2e463768a2c806dc01c331f2b88fce1646fba8e7485e6c3ff6ff50`. Извлечённый `lib/arm64-v8a/libGame.so`: 8943088 bytes, SHA256 `4c6a7445e30b27afdda781302e4db9bac89c28fc1181b68b1eef16f84d6a282e`. Обе суммы повторно проверены. ELF64, little endian, ET_DYN, machine 183/AArch64, SONAME `libGame.so`. Название версии не заменяет эти digest.

Предоставленный package inventory: `com.rockstargames.gtasa`, versionCode `4234641`, versionName `2.11.311`, minSdk 28, targetSdk 36; activity `com.rockstargames.gtasa.GameActivity`. Это декларации пакета, а не подтверждение подписи издателя. Архив содержит также `libapp.so` и `libPDALIFE.so`; назначение последнего здесь не установлено. Подпись, происхождение изменений и право распространения APK/engine/assets не проверены. Бинарники и декомпилированные тела в этот репозиторий не включены. MTA имеет собственный [LICENSE](../../LICENSE); это не лицензия на game binary.

`DT_NEEDED` libGame: `libandroid.so`, `liblog.so`, `libEGL.so`, `libGLESv2.so`, `libVendor_mpg123.so`, `libopenal.so`, `libm.so`, `libc++_shared.so`, `libdl.so`, `libc.so`. Наличие всех зависимостей и их совместимость при загрузке не проверялись. ARM64 engine нельзя загрузить в x86_64 Android H1 process; успешный headless transport там не означает работоспособность игры здесь.

## Evidence и ограничения инструмента

Локальный ignored lab: `D:/projects/astana/work/mta-mobile-lab/android-game-apk-348750d2/`. `native-members.json` связывает ZIP member, CRC extraction, ELF header и digest. `libGame-readelf.txt` — предоставленный полный ELF header/dynamic/symbol dump, raw SHA256 `cd896756e44910d5546961b0ca7b0693e3d9179c7e5fa3cca734fa8a5ce6c759`. Номера строк ниже относятся именно к этим байтам, не к C++ исходнику игры. `capabilities.json` сохраняет выбранные symbol facts без адресов вызова.

Применён skill `reverse-engineer-anything`. REA 6.7.0 CLI:

```powershell
rea inspect-binary-layout '<ignored lab>/native/libGame.so' --format json --full-output
```

Process exit был 0, но **JSON `ok:false`**: `capability_unavailable`, provider `pwntools-elf`, operation `inspect_binary_layout`, message `The initial real-verified pwntools artifact profile supports Linux x64 only.` Это отказ провайдера, не успешный AArch64 анализ. Raw envelope/stderr/exit сохранены в lab как `rea-libGame-layout.*`. Последующий безопасный `rea inspect-artifact '<library>' --format json --full-output` успешно связал ELF/digest/size с Evidence `ev_e2197366568639e07dae9d653ce12e822eced06b01f57f3cf7712662ee44e654`; `architecture:null` и explicit unexplored `open_binary` остаются ограничениями. Новых provider installs, запуска APK, debugger/Frida или вызовов native функций не было. Поэтому ниже доказаны exports/header из readelf, а не function bodies, xrefs или native call graph. Длинная декомпиляция не начиналась.

MTA source binding: commit `817dd26bbbec3c46e865780ea3642acabe71012a` в этом fork. Все source links ниже закреплены на нём.

## Карта фактов и требуемых контрактов

| Семантика | Наблюдаемый Android symbol / dump line | Реальный MTA caller contract | Вывод и неизвестное |
|---|---|---|---|
| Получить local ped | `FindPlayerPed(int)` 14863; `CPools::GetPed(int)` 15917; `GetPedRef(CPed*)` 2951 | [CGameSA.cpp:400](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/game_sa/CGameSA.cpp#L400) получает ped через pool и связывает wrapper | Кандидаты поиска ped присутствуют. Return type, local-player index, null/menu/loading lifetime и handle generation не доказаны. |
| Читать/писать позицию | `CPlaceable::GetMatrix()` 970; `SetMatrix(CMatrix const&)` 11983; `CPed::Teleport(CVector,unsigned char)` 1344; `CEntity::UpdateRwMatrix()` 11787 и `UpdateRwFrame()` 20592 | [CEntity.h:62](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/sdk/game/CEntity.h#L62), [CEntitySA.cpp:124](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/game_sa/CEntitySA.cpp#L124), [GetPositionInternal:301](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/game_sa/CEntitySA.cpp#L301) используют matrix или inline transform; reject NaN/Inf; teleport отличается от простого store | Нужны доказанные CVector/CMatrix layout, pointer lifetime и collision/world/render semantics. Символ GetMatrix не доказывает возвращаемый тип. Нельзя копировать PC offsets/structs или считать write трёх float безопасным teleport. |
| Создать stock vehicle | `CCarCtrl::CreateCarForScript(int,CVector,unsigned char)` 17494; `CStreaming::RequestModel(int,int)` 8698; `LoadAllRequestedModels(bool)` 22359; `CAutomobile` C1/C2 9087/13148 | [CPoolsSA.cpp:79](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/game_sa/CPoolsSA.cpp#L79) проверяет collision/model, вызывает CreateCarForScript, выбирает wrapper по vehicle class и регистрирует pool mapping | Предпочтительный кандидат — factory с той же семантикой, а не ручной new CAutomobile. Неизвестны ABI aggregate-by-value, factory side effects, model flags, pool capacity и ownership. Constructor symbol сам не определяет allocation size. Пока только один stock automobile, не обещание поддержки train/boat/bike/custom models. |
| Найти/удалить vehicle | `CPools::GetVehicle(int)` 2615; `GetVehicleRef(CVehicle*)` 14850; `CWorld::Add(CEntity*)` 8114 / `Remove(CEntity*)` 10438; CVehicle D0/D1/D2 7164/11395/15866; operator new/delete 4651/4248 | [CPools.h:70](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/sdk/game/CPools.h#L70), [RemoveVehicle:181](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/game_sa/CPoolsSA.cpp#L181); [CClientVehicle.cpp:3194](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/mods/deathmatch/logic/CClientVehicle.cpp#L3194) streams the game wrapper out | Remove из world не равен delete; D0/D1/D2 не взаимозаменяемы. Нужны destructor dispatch, pool invalidation, occupants/audio/physics cleanup и proof отсутствия double remove. |
| World tick | `CGame::Process()` 12120; `CWorld::Process()` 4754; `CTimer::Update()` 15187; `CPad::Update(short)` 16265 | [CClientGame.cpp:816](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/mods/deathmatch/logic/CClientGame.cpp#L816), [DoPulses:1132](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/mods/deathmatch/logic/CClientGame.cpp#L1132) координируют network, entities, Lua и render phases | Имена не доказывают порядок/поток. Нужен один наблюдаемый safe tick seam. Нельзя вызывать CWorld::Process ещё раз из network thread, создавая второй engine loop. |
| Touch/input/lifecycle | `AND_TouchEvent(int,int,int,int)` 3009; JNI implOnTouchStart/Move/End 14564/7094/22036; `CTouchInterface::Update/Clear/GetTouchDelta` 4904/2301/5117; JNI implOnPause/Resume 5277/12116; `CPad::GetPad(int)` 7424 | [CPad.h:16](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/sdk/game/CPad.h#L16) controller representation; [CClientPad.cpp:29](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/mods/deathmatch/logic/CClientPad.cpp#L29) named actions, [DoPulse:212](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/mods/deathmatch/logic/CClientPad.cpp#L212) scripted-ped state | Touch surface реально существует. Pointer-id/event encoding, coordinate transform, JNI signatures, ownership queue, pause/cancel ordering, look/aim semantics и Android pad layout ещё не установлены. CClientPad — scripted-ped handler, не готовый mobile input router. |

Обычные Itanium C++ function names не кодируют return type. Demangled spelling выше — чтение имени и параметров, не C++ prototype, который разрешено вызывать. ELF virtual address, file offset, load bias и native pointer — разные значения; map намеренно не предоставляет hook offsets или callable casts.

## Предлагаемый seam (инференс, ещё не implementation)

Сохранить server Lua, authoritative ElementID и packet semantics. В Android backend перевести уже декодированные entity operations в собственные локальные handles с generation. Таблица `server ElementID → client entity → local game handle` должна жить выше engine pool: [CClientEntity.h:155/201](https://github.com/Chelik22/mtasa-mobile/blob/817dd26bbbec3c46e865780ea3642acabe71012a/Client/mods/deathmatch/logic/CClientEntity.h#L155) хранит MTA ID отдельно. GTA pool ref, pointer и ID корреляции теста не заменяют ElementID. Stream-out уничтожает только game instance; stream-in может получить другой pointer при том же server ID. На reconnect/generation change stale operations отвергаются.

Network thread только ставит bounded commands; единственный подтверждённый game thread применяет их на safe tick. Readback копирует finite position в собственный value object; не отдаёт borrowed game memory transport/Lua. Pending model load имеет timeout/failure, spawn идемпотентен по server ID+generation, destroy повторно безопасен на уровне adapter. Ничего из этого не объявлено существующим в APK.

Input seam передаёт **semantic actions**, не Windows VK/scancodes: move X/Y, look delta X/Y, aim/fire, enter/exit, jump/sprint, steering, accelerate, brake, handbrake; отдельно UI pointer/text. Camera look и vehicle controls имеют отдельный context и приоритет UI. Snapshot несёт sequence/tick/context epoch; touch pointer-id принадлежит конкретному control до up/cancel. Pause, focus loss, Android back и IME должны сбрасывать удерживаемые действия, а resume не воспроизводить устаревшие события. Mobile UI safe-area/scale не является заменой engine CPad ABI. Контроллер опционален.

## Gates следующей вертикали

1. **A2-static**: exact digest+ELF+exports совпали. Это текущая граница; capabilities имеют `callable:false`.
2. **A3-local-read** (runtime `not_run`): отдельно разрешённый ARM64 lab с легально предоставленными assets/dependencies. Установить actual build identity, game thread/tick, ped lifetime, доказать layout/API ABI ограниченным read-only наблюдением. Position readback совпадает с контролируемым движением, null/loading/pause не падают; неверный digest блокирует backend до первого call.
3. **A3-local-world** (`not_run`): один stock model после реального streaming readiness; create/readback/destroy/recreate, pool count возвращается к baseline; отказ model load не оставляет world entry; никакого new/delete по guessed size. Телепорт проверяет collision/render/physics, а не только три числа.
4. **A3-input** (`not_run`): одновременно movement+camera и steer+accelerate+look/aim; два пальца сохраняют независимые pointer-id; cancel/up/pause/background не оставляют stuck buttons; IME/back/UI focus блокируют gameplay; safe area, density/scale, orientation, app pause/resume и потеря surface проверяются отдельно. Camera sensitivity и aim hold/toggle проверяются на устройстве; optional controller не блокирует touch baseline.
5. **A4-shared-world** (`not_run`): тот же source-only server witness и Lua resource, PC + ARM64 game backend видят один server ElementID/vehicle; реальный Lua event несёт vehicle ref, ACK от actual client source, stream-out/reconnect cleanup сходится к нулю. Headless H1 и совпавшие названия functions этот gate не заменяют.

Препятствия — ABI/layout, engine lifecycle и game/resource/render/UI platform dependencies. «Заменить команды SA-MP на MTA» недостаточно: здесь идентифицирован слой локальной engine реализации, а server protocol/Lua/entity lifecycle сохраняют собственные контракты.

## Повторить только статическую проверку

Из корня этого source checkout (Python 3.11+, без дополнительных пакетов):

```powershell
python experiments/android-engine-map/verify_inventory.py --library '<local libGame.so>' --readelf '<local libGame-readelf.txt>'
python -m unittest discover -s experiments/android-engine-map -p test_inventory.py -v
```

Первая команда сверяет exact SHA256, ELF header и 32 выбранных defined function exports по закреплённым строкам dump. Она не читает ABI из debug info и не загружает библиотеку. Вторая — шесть синтетических проверок fail-closed поведения checker, не тесты Android/gameplay. Результаты и REA envelopes сохранены в `verification/`; бинарники остаются только в ignored lab.
