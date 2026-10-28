# Trusted JSON → approved FixtureEntry vector

`generate_approved_fixture.py` — build-time Python stdlib bridge для schema `mta-fixture-approved-content-v1` из `experiments/mobile-world-fixture/content_manifest.py`. Скрипт не скачивает assets, не читает env/cache root и не является runtime JSON parser.

```powershell
python experiments/net-module/client/config/generate_approved_fixture.py --manifest <trusted-local-manifest.json> --output <build-dir>/ApprovedFixtureEntries.h
```

Header включает `FixtureContentPolicy.h` через include path HTTP target и определяет:

```cpp
namespace experimental_client::approved_fixture
{
    inline std::vector<experimental_http::FixtureEntry> Entries();
}
```

Root keys обязательны и точны: `schema`, `runtime` (`not_run`), `entries`. Каждая entry содержит только `destination_relative`, `url`, `raw_sha256`, `max_bytes`, `format`. Input bounded1MiB, entries1..128. Duplicate JSON keys, unknown fields, bool вместо integer, невалидные hashes/paths/URLs/formats отвергаются до записи output. SHA256 нормализуется в lowercase, entries сортируются по UTF8 destination bytes. Нет timestamps или input filenames в header; output детерминированный.

Destination reuse базовой `content_manifest.relative_path` дополняется лимитом4096UTF8 bytes, control/device-alias checks и запретом case-insensitive duplicates. URL — ASCII HTTP(S), без credentials/query/fragment/unsafe chars, с проверенным host/port/percent encoding и без decoded traversal/encoded separators. Не-ASCII URI требуется percent-encode. Это намеренно строгий subset слабее проверяемых аргументов producer; вывод actual producer с обычным loopback URL принят tests.

Formats: `binary_asset` → `FixtureFormat::BinaryAsset`, bound0..256MiB; `lua_source_utf8` → `FixtureFormat::LuaSourceUtf8`, bound0..1MiB, согласно current C++ policy. Producer обычно выдаёт `binary_asset` до64MiB. Digest здесь проверяется синтаксически: доверие к его происхождению обеспечивает автор локального manifest, а загруженные bytes проверит существующая policy.

Каждый byte destination/URL/hash выдаётся fixed3-digit octal escape внутри обычного C++ string literal. Raw input не попадает в C++ source даже при разрешённых UTF8/apostrophe/percent-encoded quote. Генерация атомарная; invalid input возвращает nonzero и сохраняет прежний header. CMake должен зависеть от JSON, generator и producer Python script и останавливать build при ошибке. CMake wiring/client factory/cache-root остаются за координатором.

```powershell
python experiments/net-module/client/config/test_generate_approved_fixture.py -q
```

Actual Python positive/negative/CLI tests выполнены. Нативная компиляция generated header и HTTP/Core runtime — not_run. Tests создают только временные fixtures в ignored `config/build`; никакого SDK/AVD/game запуска.
