# A2: version-bound read-only player position

Статические инструкции подтверждают путь чтения позиции **без `GetMatrix()`**: matrix pointer по `this+0x18`; при null — три float32 по `this+0x08/+0x0c/+0x10`, иначе — по `matrix+0x30/+0x34/+0x38`. `CPed::Teleport` подтверждает эти же поля относительно исходного CPed this, без adjustment. Это факты для конкретного бинарника, **не разрешение runtime вызова**: `callable=false`, native/game readback `not_run`.

## Binding и метод

`lib/arm64-v8a/libGame.so`, 8943088 bytes, ELF64 little-endian AArch64, SHA256 `4c6a7445e30b27afdda781302e4db9bac89c28fc1181b68b1eef16f84d6a282e` повторно проверен перед анализом. APK identity из предыдущего inventory: `348750d29d2e463768a2c806dc01c331f2b88fce1646fba8e7485e6c3ff6ff50`, version `2.11.311`; APK на этом шаге не хэшировался/не распаковывался/не запускался.

REA skill применён. Сохранённая успешная safe artifact identity `ev_e2197366568639e07dae9d653ce12e822eced06b01f57f3cf7712662ee44e654` связывает тот же digest. Известный deep ELF provider не поддерживает AArch64; setup/deep retries не делались. Fallback — существующий NDK28.2 LLVM llvm-objdump, только bounded function ranges; llvm-readelf — relocations. Шесть functions суммарно **544 bytes**. Никаких compiler/GUI/native function execution, hooks или memory writes в процессе игры.

Raw bodies, relocations и полные команды только в ignored lab `D:/projects/astana/work/mta-mobile-lab/android-game-apk-348750d2/position-static-001/`. `observations.json` содержит собственные факты и hashes/anchors, без function bodies, machine code, guessed typedef или game class implementation. Адреса ниже — ELF analysis virtual addresses/RVA, не file offsets и не готовые process pointers. Runtime load bias/relocations требуют отдельной проверки.

## Подтверждено инструкциями

| Function | Bounded range | Наблюдение и anchors |
|---|---|---|
| `FindPlayerPed(int)` | `0x4af7d4..0x4af818` (68 bytes) | Нет BL/BLR и stores. Signed-negative argument выбирает focus byte; неотрицательный — сам index (`0x4af7dc/0x4af7fc`). Строка PlayerInfo имеет stride `0x1d8` (`0x4af7e8/0x4af804`), result — 64-bit load из начала выбранного slot (`0x4af810`), возвращается в x0. Bounds check отсутствует. |
| `CPlaceable::GetMatrix()` | `0x453f84..0x453fc0` (60 bytes) | Читает pointer `this+0x18` (`0x453f94`); при null вызывает `AllocateMatrix` и `CSimpleTransform::UpdateMatrix` с transform по `this+0x08` (`0x453fa0/0x453fa8/0x453fac`). Поэтому этот getter может **аллоцировать и менять game state**. |
| `CPlaceable::CPlaceable()` C1/C2 alias | `0x4acadc..0x4acaf4` (24 bytes) | Записывает vtable pointer в начало и обнуляет bytes8..31 (`0x4acae4/0x4acaec`). Это footprint конструктора, не доказанный sizeof CPed/полной object layout. |
| `CPlaceable::IsWithinArea(4 floats) const` | `0x4acdf0..0x4ace40` (80 bytes) | Читает pointer `this+0x18`, выбирает inline `this+0x08` при null или `matrix+0x30` (`0x4acdf0..0x4ace00`), затем два float32 (`0x4ace14`). Нет calls/stores. |
| `CPlaceable::IsWithinArea(6 floats) const` | `0x4ace40..0x4aceb0` (112 bytes) | Тот же storage selector (`0x4ace40..0x4ace50`); float32 components0/1 загружаются по selected address, component2 — ещё через8bytes (`0x4ace70/0x4ace84`). Нет calls/stores. |
| `CPed::Teleport(CVector,unsigned char)` | `0x590378..0x590440` (200 bytes) | Сохраняет исходный this в x19 (`0x59039c`), читает `x19+0x18` (`0x5903d0`). При nonnull пишет три input float components в `matrix+0x30/+0x34/+0x38` (`0x5903d8/0x5903e0/0x5903e8`); при null — в `this+0x08/+0x0c/+0x10` (`0x5903f0/0x5903f4`). Для этих обращений this не корректируется. Также меняет tasks/world/references/другое state: не использовать для чтения. |

Итак, storage relation CPed→CPlaceable для указанных fields подтверждена body CPed method, а не взята из Win32 MTA. Полная hierarchy/vtable/sizeof CPed или CMatrix этим не восстановлены.

## FindPlayerPed globals: подтверждено relocations

| GOT slot RVA | Symbol и собственное значение в ELF | Использование |
|---|---|---|
| `0x837378` | `CWorld::PlayerInFocus`, symbol `0xa1a8d0`, size1 | Byte используется вместо отрицательного index. |
| `0x839a08` | `CHID::currentInstanceIndex`, symbol `0xd0df30`, size4 | При значении1 result выбирает **второй** PlayerInfo slot независимо от index. Это не CGameLogic::GameState. |
| `0x837358` | `CWorld::Players`, symbol `0xa1a520`, size944 | Base для выбора slot; 944 =2×472. |

Это `R_AARCH64_GLOB_DAT` bindings из pinned ELF. Live GOT содержит relocated pointers, не эти static numeric values. Size944/stride472 поддерживают интерпретацию двух PlayerInfo slots, **не** количества multiplayer players. Произвольный index функция не ограничивает; server ElementID/SA-MP player ID нельзя передать ей как slot. Return pointee interpretation как CPed — ABI inference по назначению экспортированной функции и проверенному CPed prefix; actual native object identity/lifetime ещё не наблюдались.

## ABI inferences и неизвестное

Сильный inference: три ordered float32 components — XYZ position; selector совпадает в read-only area predicates и в CPed teleport writes. AArch64 x0 несёт this/return pointer, w0 — int selector; float operands S0..S2 в Teleport соответствуют компонентам CVector. Это **не callable C++ prototype** и не обещание совместимости Win32 structs.

Неизвестно: live module/load bias/GOT bindings, состояние после JNI_OnLoad/модификаций APK, действительность CPed pointer/его subclass identity, момент появления/удаления local ped, доступность mapped memory и matrix lifetime, concurrent engine workers, атомарность snapshot, world units/границы, interior/dimension, server entity mapping. Static methods не доказывают safety dereference во время loading/background/teardown. Матрица может появляться/исчезать, а два последовательных чтения сами по себе не удерживают объект от удаления.

## Следующий конкретный read-only backend probe

После настоящего APK boot и наблюдаемого `GameNative` frame на **том же GameThread**, внутри фактической ветки `hasFocus && implIsInitialized`:

1. Проверить live mapped library identity против pinned file и выбранных code anchors, actual ELF architecture/load bias и экспортированный symbol address. File SHA не доказывает отсутствие runtime patching сторонней library; несовпадение блокирует probe.
2. При loading/pause/surface loss/teardown probe не выполнять. Candidate — `FindPlayerPed(-1)` один раз в разрешённом frame, без произвольного index. До вызова нужны реальные lifetime/thread guards; текущий contract оставляет callable=false.
3. Null result — диагностическое отсутствие ped, не успех position read. Ненулевой pointer проверить как ожидаемый native ped и readable span. Затем использовать подтверждённый selector: прочитать pointer+0x18, выбрать inline position+0x08 либо matrix position+0x30; скопировать только три float32 в собственный value. **Не вызывать GetMatrix или Teleport.**
4. Finite-check, проверки стабильности ped/matrix identity и frame/lifecycle epoch; изменившийся/невалидный snapshot отбросить. Такие проверки не заменяют доказательство object lifetime: при неизвестном concurrent ownership чтение остаётся blocked.
5. Readback сравнить с реальным local motion на устройстве и текущим состоянием игры. Только после этого разрешать local engine read capability; A3 world sync, Lua/RP/entity IDs и U1 touch/gameplay остаются отдельными проверками.

PC MTA `CEntitySA::GetPositionInternal` (source pinned в предыдущем A2 map, Client/game_sa/CEntitySA.cpp301) тоже выбирает matrix либо inline transform, но это сравнение семантики. Его pointer widths/offsets переносить нельзя; GetPosition дополнительно может восстанавливать physics при bad position. Наш будущий read-only probe при ошибке возвращает отсутствие валидного sample, не исправляет game state и не переписывает серверные Lua ресурсы.
