# PC baseline: CLI preparation

Это подготовка следующего настоящего PC baseline. Полный клиент MTA ещё не
собран и не запускался; G1 остаётся `not_run`. GUI, GTA, установленная MTA и RP
не запускаются этими командами. `build` — отдельная команда после согласования
исполнения с владельцем кампании. Подготовка не заменяет игровой gate.

Baseline checkout создан от `7ccf8e118b0e856dd2b17ee879f508132d3ca49d`.
`Client/Server/Shared/vendor/utils/premake5.lua` равны upstream
`a84e0c77c66f596b0c72def1c857253568208a2b`. Opt-in experimental Core в этот
baseline не включён. Новые tracked файлы находятся только в этой директории.

## Фактически доступные prerequisites

- VS2026 Community, MSVC v145 / 19.51.36257.0, Windows SDK 10.0.26100.0,
  x86 compiler/libs; Premake и `gen_language_list.exe` уже были в checkout.
- CEF Windows32 152.0.5, Discord v3.4.3, RapidJSON v1.1.1 и Unifont v16.0.04
  загружены по pinned upstream URLs/hashes и материализованы в ignored build
  dependency directories. Ни один dependency installer не запускался.
- Полного MFC installation нет. Официальный VS catalog предоставил отдельный
  `Microsoft.VC.14.51.MFC.Headers.base` VSIX. Проверенный header tree находится
  в lab; `afxres.h` нужен только resource compiler. Отдельный MSBuild props
  добавляет этот include path. ATL уже установлен. Глобальная VS не изменялась.
- Microsoft.DXSDK.D3DX 9.29.952.8 взят с NuGet; SHA512 сверялся с официальной
  package metadata. Scoped layout содержит настоящие headers/import libraries.
  Windows SDK предоставляет остальные D3D9/DirectInput/XInput headers/libs.
- `dxerr.lib` собран из неизменённого MIT source Microsoft DirectX-SDK-Samples,
  pin `07e3eaa10e7dd026ec9d95fe326db2d5c4227e1b`. Реальный archive — I386,
  три public stdcall symbols подтверждены `dumpbin`; probe со всеми тремя
  функциями успешно linked, но не запускался. Это Unicode-only DxErr с
  buffer-based `DXGetErrorDescriptionW`, общей legacy ABI совместимости нет.
  У baseline MTA найден только `links "dxerr.lib"`; used-symbol set пустой.

Полный legacy DirectX SDK installer не используется. Автоматическая проверка
инструмента отклонила составной PowerShell download/hidden-process/signature шаг
для официального `DXSDK_Jun10.exe`, без подробной причины. Он не выполнялся и не
повторялся. Принятый путь использует NuGet SDK files и настоящий Microsoft source.

Данные cache/SDK, archive/tree manifests, native objects, executables и полные
логи остаются в lab вне Git. Snapshot содержит только metadata/hashes.

## Повторение подготовки

Перед любым новым инструментом проверь skills и существующие CLI. Здесь
используются уже установленные Python, CMake, Premake, MSBuild и Windows SDK.
Проверь минимум 8 GiB свободного диска. Не меняй глобальные PATH/registry/VS.

```powershell
$cache = 'D:/projects/astana/work/mta-mobile-lab/pc-dependencies'
python experiments/pc-baseline/prepare_dependencies.py --cache $cache --download
```

Для чистого cache получи шесть source files по `dxerr-source.lock.json`:

```powershell
@'
import hashlib, json, urllib.request
from pathlib import Path
root = Path('D:/projects/astana/work/mta-mobile-lab/pc-dependencies/dxerr-source-audit')
root.mkdir(parents=True, exist_ok=True)
lock = json.loads(Path('experiments/pc-baseline/dxerr-source.lock.json').read_text())
for item in lock['files']:
    path = root / Path(item['path']).name
    data = path.read_bytes() if path.exists() else urllib.request.urlopen(item['url'], timeout=45).read()
    assert hashlib.sha256(data).hexdigest() == item['sha256'], item['path']
    path.write_bytes(data)
'@ | python -
$env:_CL_ = '/MP1'
cmake -S experiments/pc-baseline/dxerr -B "$cache/dxerr-build" `
  -G 'Visual Studio 18 2026' -A Win32 `
  "-DDXERR_SOURCE_ROOT=$cache/dxerr-source-audit" `
  "-DD3DX_INCLUDE_DIR=$cache/d3dx-extracted/build/native/include"
cmake --build "$cache/dxerr-build" --config Release --parallel 1 -- `
  /nr:false /p:MultiProcessorCompilation=false
```

Остановись при nonzero exit. Проверь I386 archive и symbols через установленный
`dumpbin /headers` и `/linkermember:1`. Probe запускать не требуется.
Scoped SDK layout состоит из verified D3DX include/x86 libs и построенного DxErr:

```powershell
@'
import shutil, sys
from pathlib import Path
sys.path.insert(0, 'experiments/pc-baseline')
from build_pc import copy_verified_tree
cache = Path('D:/projects/astana/work/mta-mobile-lab/pc-dependencies')
sdk = cache / 'scoped-dxsdk'
copy_verified_tree(cache / 'd3dx-extracted/build/native/include', sdk / 'Include')
copy_verified_tree(cache / 'd3dx-extracted/build/native/release/lib/x86', sdk / 'Lib/x86')
destination = sdk / 'Lib/x86/dxerr.lib'
if destination.exists():
    raise SystemExit('Preserve and inspect existing SDK output before replacing it')
shutil.copy2(cache / 'dxerr-build/Release/dxerr.lib', destination)
'@ | python -
python experiments/pc-baseline/build_pc.py prepare --cache $cache `
  --dxsdk "$cache/scoped-dxsdk" `
  --plan D:/projects/astana/work/mta-mobile-lab/pc-client-plan-001.json
```

`prepare` проверяет pinned source/dependency bytes, копирует только cache
dependencies, вызывает `premake5 vs2026` и единственный
`ValidateSolutionConfiguration` для проверки реальных имён targets.
Ни один MTA project build target при подготовке не исполняется. Проект/solution
hashes, dependency graph, resource props и SDK hashes фиксируются в plan.
После commit helper-кода повтори prepare, чтобы source SHA плана совпадал с HEAD.

## Настоящая компиляция: отдельный следующий шаг

```powershell
python experiments/pc-baseline/build_pc.py build `
  --plan D:/projects/astana/work/mta-mobile-lab/pc-client-plan-001.json `
  --run-root D:/projects/astana/work/mta-mobile-lab/pc-client-build-001
```

Запускай один раз в сохраняемой CLI session, следи за тем же PID/logs до terminal
результата. Перед повтором проверь lock и child PID. Helper не имеет короткого
build timeout и не убивает чужие процессы. При прерывании runner живой child
остаётся в manifest/lock для адресного наблюдения.

Targets проверены по фактическому MSBuild metaproject: `Client\Client Launcher`,
`Client\Loader`, `Client\Loader Proxy`, `Client\Client Core`, `Client\Game SA`,
`Client\Multiplayer SA`, `Client\GUI`, `Client\Client Deathmatch`,
`Client\Client Webbrowser`, `Client\CEFLauncher`, `Client\CEFLauncher DLL`,
`Shared\XML`. Пробелы сохраняются. Их 34 project dependencies не включают сервер.
Не используй solution-wide Build или Rebuild. Это холодная Win32 сборка примерно
1540 declared translation units с тяжёлыми PCH/CEF/Crypto++; длительность ещё не измерена.

Release/Win32, `/m:1`, `/p:MultiProcessorCompilation=false`, `_CL_=/MP1`,
`/nr:false`, hidden console и below-normal priority. Перед компиляцией нужно
не менее 2 GiB свободной физической RAM и 8 GiB commit headroom. Успех компиляции
даёт `clientBuild=passed`; G1 остаётся `not_run` до настоящей игры/Lua evidence.

## Runtime prerequisites остаются отдельными

Реестр указывает установленную MTA 1.6 и GTA path с существующими exe/models/data/audio.
Они не изменяются. Static REA evidence выбранного GTA exe подтверждает x86 и digest,
но version resource отсутствует: нужная GTA:SA 1.0 пока не подтверждена.
Source gate: `Client/core/CCore.cpp::CreateGame`, версии >=1.1 отвергаются.
Отдельно нужны официальный совместимый `netc.dll` 1.7 и полный app-local data
runtime; установленный netc от MTA 1.6 автоматически не переиспользуется.
`win-install-data.bat` скачивает несколько сетевых модулей и потому здесь не вызывается.
Перед GUI/GTA запуском владелец кампании предупреждает пользователя и выбирает
изолированный runtime. Build preparation не доказывает graphics/physics/client Lua.

Проверки подготовки: `python experiments/pc-baseline/test_preparation.py -v`.
Это filesystem/provenance/preflight tests, не MTA gameplay.

Источники: [MTA build guide](https://wiki.multitheftauto.com/wiki/Compiling_MTASA),
[Microsoft D3DX package](https://www.nuget.org/packages/Microsoft.DXSDK.D3DX/9.29.952.8),
[Microsoft source/license pin](https://github.com/microsoft/DirectX-SDK-Samples/tree/07e3eaa10e7dd026ec9d95fe326db2d5c4227e1b).

## Отдельная gameplay integration в том же cache

`integration_build.py` использует отдельный schema/receipt: он не ослабляет
upstream-only проверку `build_pc.py`. Baseline branch566558 и immutable capture
сохранены. Integration разрешает только compile trees reviewed featurebbe02075b,
фиксирует13 source deltas, physical input set, прежний34-project graph и scoped
SDK exact map. Prepare ничего не генерирует и не компилирует. Объекты не копируются;
используется тот же worktree/cache. Отдельный explicit `build` запускает12targets,
Normal priority, hidden, `/m:1`, `/p:MultiProcessorCompilation=false`, `_CL_=/MP1`,
`/nr:false`, с fresh physical2048MiB/commit8192MiB и отказом при активном emulator/
compiler. Требуются изменившиеся core/loader/proxy outputs; G4/gameplay остаются
not_run независимо от успеха сборки. Полный command и source/dependency hashes
фиксируются в новом integration plan вне Git. Проверки отказа до Popen:
`python -B experiments/pc-baseline/test_integration.py -v`.
