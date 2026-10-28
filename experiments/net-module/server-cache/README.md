# Server HTTP cache filename policy — узкий G4 fixture

Причина actual preflight001: CResource.cpp641 вызывает CNetServer::ValidateHttpCacheFileName перед проверкой cached checksum/FileCopy. OwnedNetServer не переопределял SDK default false, поэтому actual server отклонил `client.lua` и `http-witness.txt`, а mobile-world-fixture не загрузился. Process38800 завершён штатной shutdown, exit0; сам preflight failed из-за отсутствующего resource_start. Raw captures/hashes — `verification/preflight-001/`. Клиент/GTA там не запускались, G4 не пройден.

`CResourceFile.cpp116–121` формирует абсолютный SDK UTF8 путь:

| Resource item | Relative path внутри resource-cache |
|---|---|
| `<file src="http-witness.txt"/>` | `http-client-files/mobile-world-fixture/http-witness.txt` |
| `<script src="client.lua" type="client" cache="false"/>` | `http-client-files-no-client-cache/mobile-world-fixture/client.lua` |

Source-only override в OwnedNetServer использует эти две fixture entries. Algorithm получает trusted allowlist отдельно, не содержит wildcard для произвольного resource. Это **filename/path validation**, не private official scan, content signature, checksum approval или общая RP compatibility. Содержимое файла эта policy не читает; source/cached checksum остаётся настоящему CResource, client approved-content policy остаётся отдельной.

До InitNetServerInterface задать process-only `MTA_EXPERIMENTAL_SERVER_HTTP_CACHE_ROOT` в **существующую canonical absolute directory** `<isolated server>/mods/deathmatch/resource-cache`. Runtime preparation должна создать эту directory заранее. Constructor замораживает env/root/allowlist один раз; последующее изменение env не расширяет уже созданный instance. Отсутствующая/пустая/invalid root оставляет validator disabled(false), но не прерывает CNetServer Init/H1 transport: source-only H1 этот API не вызывает. Никакого fallback к cwd/installed server и никаких filesystem writes при constructor/validation.

Path contract: max4096 UTF8 bytes, local absolute path (Windows local drive; no UNC/device prefix), canonical spelling без dot/dotdot/repeated/trailing separator; portable segments без invalid chars/controls/trailing dot/space/DOS device/tilde8.3 aliases. Root допускает корректный UTF8 и на Windows преобразуется через CP_UTF8/strict flags, не ACP. Allowlist fixture filenames ASCII, max128 entries, case aliases/duplicates отвергаются; candidates сравниваются с exact-case relative key. Два fixture keys не превращаются в разрешение всех `.lua`/`.txt`. Root path spelling/case должно совпадать с фактическим SDK path; trailing slash не нужен.

Final destination может ещё отсутствовать: проверка стоит **до** FileCopy. Все existing ancestors проверяются на directory и отсутствие symlink/reparse (Windows GetFileAttributes, включая junction). Existing final item должен быть regular file, не directory/link. Root повторно проверяется каждый call; weakly_canonical не должен изменить candidate. Missing suffix разрешён только для exact allowlist entry. Unknown/malformed paths возвращают false, C++ exceptions не выходят из Allows.

Граница интерфейса: bool filename API не может удержать filesystem handle до последующего caller IO. Это check-time проверка, не atomic confinement от concurrent directory replacement. Дополнительно CResource.cpp643 при reject сам вызывает FileDelete по исходной строке: validator этого delete не контролирует. Trusted resource meta, isolated root и отсутствие внешних writers обязательны для этого fixture lab; строгая adversarial защита требует отдельного caller repair/handle-aware IO. Policy не обещает закрыть этот известный caller эффект.

## Проверка и wiring

Header-only, использует standard C++20 filesystem и Windows API при WIN32; ни client CNet headers, ни HTTP/curl backend не импортируются. Узкий OwnedNetServer.cpp override + const member; base virtual signature/ABI остаются прежними. Root CMake верхнего уровня не менялся. Для root suite можно `add_subdirectory(server-cache)`; отдельно:

```powershell
cmake -S experiments/net-module/server-cache -B '<ignored lab>/server-cache-tests' -G 'Visual Studio 18 2026' -A x64
cmake --build '<ignored lab>/server-cache-tests' --config Release --parallel 1 -- /m:1 /nr:false /p:MultiProcessorCompilation=false
ctest --test-dir '<ignored lab>/server-cache-tests' -C Release --parallel 1 --output-on-failure --no-tests=error
```

Portable tests: missing target для обоих buckets, regular files, unknown key/wrong bucket/wrong case, prefix escape/traversal/aliases/invalid UTF8/size/null, bad roots/entries, UTF8 root, frozen process env, directory final/file ancestor. Отдельный symlink test требует real OS symlink; при отсутствии права выходит77 и CTest явно SKIP, не PASS доказательство reparse. Linux symlink и Windows reparse/junction source branches присутствуют, но отсутствие actual run нельзя заменять source review.

На этом source commit новая policy/tests/native DLL **не компилировались и не запускались**. Root review → один Win64 net_experimental DLL + policy tests build с fresh memory guard → новый actual isolated server preflight: resource_start, console fixture create/dispose, HTTP response bytes; затем настоящий game client отдельно. Failed001 captures сохраняются, новых server/game процессов здесь не создавалось.
