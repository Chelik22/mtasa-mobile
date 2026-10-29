# Pinned Android position candidate

Первый source sampler для конкретного `libGame.so`2.11.311. Он читает реальные
адреса текущего процесса через kernel copy; никакие игровые функции не вызываются,
память игры не изменяется. **Host boundary tests и ARM64 final link прошли;
Android runtime, game-thread integration и object lifetime пока не проверены.** Возвращаемый результат называется
`candidate_unverified`, пригодного для gameplay значения API не выдаёт.

## Что реализовано

- `AndroidModule::Open` на Android ARM64 находит единственный загруженный
  `libGame.so` через `dl_iterate_phdr`. Требуется PT_LOAD c offset/vaddr0.
- Открывает его фактический absolute file path с O_NOFOLLOW. Inode/device первого
  readable mapping с offset0 должны совпасть с открытым FD. Для packed APK library
  (`!`), deleted mapping, unreadable `/proc/self/maps` и относительного linker name
  возвращается явный отказ, без выбора похожего extracted файла.
- Проверяет8943088bytes и полный SHA256
  `4c6a7445e30b27afdda781302e4db9bac89c28fc1181b68b1eef16f84d6a282e`
  существующей реализацией `Shared/sdk/sha2.hpp`; file stat до/после hash должен
  совпасть. File/live ELF header — ELF64 little-endian AArch64 ET_DYN.
- Проверяет live FindPlayerPed anchor RVA0x4af7d4/68bytes против fingerprinted
  файла, вычисляя file offset через executable PT_LOAD. Game bytes в Git нет.
- Для каждого candidate до/после проверяет mapping/file metadata/code anchor.
  Decoder требует точные три GOT pointers:
  `0x837378→bias+0xa1a8d0`, `0x839a08→bias+0xd0df30`,
  `0x837358→bias+0xa1a520`. Другой relocation/interposition отвергается.
- Semantics `FindPlayerPed(-1)` реализованы чтением: instance word==1 выбирает
  slot1, иначе focus byte; только slot0/1, stride0x1d8. Никакой server ElementID
  не передаётся как slot. Matrix pointer ped+0x18 выбирает12bytes по ped+8 или
  matrix+0x30. Три компоненты должны быть finite; null ped — `no_player`.
- `process_vm_readv(getpid)` копирует максимум4096bytes за вызов, без прямого
  dereference; partial/denied/ENOSYS — `read_unavailable`. В установленном NDK
  `sys/uio.h` API объявлен сAPI23, build target здесьAPI28+. Разрешение SELinux и
  ARM translation runtime не предполагается.
- Повторное чтение slot/selector/matrix отбрасывает замеченные изменения. Failure
  всегда очищает output, чтобы предыдущая позиция не стала текущей.

Offsets и semantics взяты из [position map](../android-position-map/README.md),
не из PC layout. Decoder tests используют независимые golden memory values и
проверяют matrix replacement, invalid pointers/GOT/selector, partial read, null,
NaN, inline/matrix branch и instance override. Это synthetic boundary tests,
не доказательство Android исполнения.

## Граница интеграции

Экспортов JNI, hooks, вызова native game методов и публичного `verified=true` нет.
`ReadUnverifiedCandidate` — диагностический кандидат, а не local-player API.
Следующий шаг — подключить собственный consumer в фактический ExecutorThread
перед `GameNative.implOnDrawFrame(F)V`, внутри реальных focus/initialized gates,
с lifecycle epoch и идентичностью именно этого потока. Вход из UI thread или
фоновый polling не заменяют этот callback. Пока seam не подключён, sampler
самостоятельно не вызывается и данные на сервер не отправляются.

Kernel copy избегает user-space invalid dereference, но **не удерживает CPed или
matrix от удаления**. Повторное чтение не обнаруживает ABA, не делает12bytes
атомарными и не доказывает согласованность координат. Hash файла и anchor не
доказывают отсутствие всех runtime patches APK. Потребуются реальное наблюдение
lifetime, game thread/phase, loading/pause/teardown и сравнение с motion readback;
до этого gameplay capability остаётся запрещённой. Для APK-backed mapping нужен
отдельный verifier ZIP segment/FD identity, а не снятие текущей проверки.
Mapping проверки также не исключают unload/remap между двумя наблюдениями или
повторное отображение того же inode/bias (ABA). FD удерживает файл, не library
mapping и не игровые объекты. Последующие mapping/code checks не являются lease.

## Сборка после разрешённого resource preflight

```powershell
cmake -S experiments/android-position-backend -B <fresh-host-build>
cmake --build <fresh-host-build> --config Release --parallel 1
ctest --test-dir <fresh-host-build> -C Release --output-on-failure
cmake -S experiments/android-position-backend -B <fresh-arm64-build> `
  -DCMAKE_TOOLCHAIN_FILE=C:/Android/ndk/28.2.13676358/build/cmake/android.toolchain.cmake `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DBUILD_TESTING=OFF
cmake --build <fresh-arm64-build> --parallel 1
```

Воспроизводимый Windows CLI runner дополнительно
проверяет настоящий final shared ELF link, единственный diagnostic export и
разрешение global imports по установленным API28 libc/libdl/libm stubs:

```powershell
python experiments/android-position-backend/build_probe.py `
  --expected-source <exact-clean-40-character-commit> `
  --run-root D:/projects/astana/work/mta-mobile-lab/android-position-build-001
```

Run root должен быть новым и вне checkout; повторный запуск в нём запрещён.
Каждый process получает отдельные stdout/stderr, PID, exit и receipt, который
обновляется атомарно. Source/tool SHA и clean commit проверяются перед каждым
stage; host tests не считаются выполненными только по build exit. Перед каждым
stage нужен physical RAM1536MiB/commit8192MiB и отсутствие другого compiler/VM.
Порог1536 согласован root для этого маленького single-worker TU/link slice,
не применяется к VM или полной сборке MTA. Runner не устанавливает tools,
не меняет registry/ADB и не запускает итоговую Android library. Final probe
не экспортирует sampler/JNI: единственный `MtaPositionCandidateBuildAbi` возвращает
число28; `--no-gc-sections` сохраняет реальные backend зависимости при link.
Форматирование только собственных5C++ файлов выполнено существующим NDK
clang-format. Repo wrapper `utils/clang-format.ps1` требует скачивания отдельного
tool21 и обходит Client/Server/Shared/Tests, не experiments; он не запускался,
новая утилита не устанавливалась. `git diff --check` прошёл.

## Native build001: 2026-10-30

На frozen source `c2d8c2459d57ba56f55627169bf651a9294f7413` выполнен настоящий
CLI run001: host configure/build/test exit0, CTest1/1 passed; NDK28.2 ARM64/API28
configure/final shared link exit0, три собственных translation units. Девять
native process stages terminal exit0. Raw receipt/logs только в ignored lab
`D:/projects/astana/work/mta-mobile-lab/android-position-build-001`.
[Metadata receipt](verification/build-windows-arm64-2026-10-30.json) связывает
source/tool/log hashes и independent-readable результаты inspection.

Final ELF64/little/AArch64 DYN2589880bytes SHA
`8ff6b06842b57da8fb4514188481372e6815edb13b26affbf9c669873cff5a6b`,
единственный defined global export `MtaPositionCandidateBuildAbi`. NEEDED только
libc/libdl/libm; все global imports разрешились по фактическим API28 stubs,
включая process_vm_readv/dl_iterate_phdr/pread/fstat. Own EXE/SO captured read-only
в ignored artifacts, в Git не включены. Compiler stderr пустой; warnings в
stdout нужно оценивать по raw logs, а не считать все сборки warning-free по
успешному exit. Shared library не загружалась/не выполнялась. Это compile/link
proof, не runtime readback или разрешение gameplay capability.
