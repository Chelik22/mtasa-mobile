# Изолированная сборка экспериментального Core

Собирается только `Server/Core`, Release/x64, из opt-in loader revision `128f7012508c9319d752fb9e8232b5f06adf0130`. Primary baseline worktree и его MSBuild не затрагиваются. `Server/core` не редактируется; objects/libs из baseline не копируются.

## Текущее исполненное состояние

На 2026-10-28 helper прошёл Python syntax check, но первая попытка inspection остановилась на RAM gate: 1669 MiB вместо необходимых 2048. Однократный последующий замер дал 560 MiB. **Premake, Core compile и server runtime не запускались.** Snapshot: `verification/deferred-2026-10-28.json`.

Статически проверены pinned `Server/core/premake5.lua` и root `premake5.lua`: девять Core CPP, SharedLib, C++23/v145/static runtime, x64 output. Собственные Windows links в Core не объявлены; Linux/macOS links не относятся к Windows. Идентичность build definitions primary worktree подтверждена. Его уже существующий generated Core project прочитан только как proxy cross-check: refs/Release-x64 additional dependencies пусты. Собственный generated project ещё требует inspection после освобождения RAM; proxy не является build evidence.

## Запуск

```powershell
python experiments/net-module/core/build_core.py --inspect-only
python experiments/net-module/core/build_core.py
```

Сначала helper проверяет compile-input trees относительно pin, свободную RAM не менее 2 GiB и генерирует solution установленным `premake5 vs2026`. Manifest сохраняет фактические `Core.vcxproj` inputs, project references и linker dependencies. Heavy gameplay references блокируются до компиляции. После inspection RAM проверяется повторно.

MSBuild получает только `/t:Server\Core`, `/m:1`, `/nr:false`, `MultiProcessorCompilation=false`; `_CL_=/MP1` ограничивает generated compiler multiprocessing. Процессы имеют BELOW_NORMAL priority и CREATE_NO_WINDOW. Внутреннего короткого timeout у сборки нет: наблюдай один PID и его логи, не создавай дубликат.

MSBuild exit 0 сам по себе не означает новый Core. Helper сохраняет before/after DLL hash/mtime; неизменённый либо предшествующий попытке файл даёт `reused_artifact`, а не passed. Если действительно требуется свежая повторная сборка, `--rebuild-core` явно выбирает только `Build/Core.vcxproj /t:Rebuild`, без whole-solution rebuild. Этот режим не запускался.

Только после fresh output и input-trace checks записываются H1-compatible `core-build.run.json` (`sourceSha`, `targets`, `trackedDirty`, фактический MSBuild `pid`/`startedUtc`, artifact hashes) и `core-build.exitcode=0`. Новая попытка сначала инвалидирует старую readiness; inspect/deferred/no-op не дают exit marker 0. Необходим чистый tracked worktree, поэтому изменения helper/source сначала коммить.

По умолчанию результат, PID шагов, команды, source/tool hashes и логи находятся в ignored `experiments/net-module/core/build`. `actual-read-inputs.json` сохраняет обнаруженные compiler/linker read dependencies и hashes; исходные tlogs остаются в ignored Build. Артефакт — `Bin/server/x64/core.dll`. Это не полноценный runtime и не доказательство загрузки Core/NetServer; сервер helper не запускает.

При RAM ниже порога сохраняется `deferred_memory`, без нового compile. Active lock не удаляется автоматически после внешнего убийства runner: сначала проверь записанный дочерний PID/логи. Runtime остаётся `not_run` даже при успешной компиляции.

Последнее распоряжение кампании: target запускает только root; изолированный worktree пока остаётся notbuilt. После resource pressure root может выбрать обычный incremental Core build в primary после сохранения B0 snapshot; это отдельное действие и не выполнено этим helper.

`python experiments/net-module/core/test_metadata.py -v` проверяет только metadata freshness (включая no-op и старый output), без запуска compiler/DLL/server.
