# Changelog

- 2026-10-28: добавлен отдельный cache-only Android x86_64/API26 CMake entry point для исходных H1 client, MtaBitStream/ModuleRuntime и codec/protocol executables. Configure отделён от сборки; native runtime и host interaction пока not_run.
- 2026-10-28: закрыты extra/symlink shadow inputs ENet и lifetime edge configure child при ошибке manifest; actual configure exit0,14 translation units, application build не запускался.
- 2026-10-28: подготовлен persistent single-worker Ninja runner с source/dependency freeze checks, RAM gate2048MiB, PID/log/exit evidence и будущей ELF inspection. Выполнен только preflight; HTTP standalone сохраняет первый memory slot.
- 2026-10-28: после independent review замкнут hash Ninja include/subninja closure и точный file set include trees, включая ignored/untracked additions; семь filesystem negative checks и actual no-Run preflight passed. Cold compile не выполнялся.
- 2026-10-28: первый actual NDK build failed на SDK/libc++ filesystem namespace conflict; failed receipt сохранён. Подготовлены scoped libc++ header fix, separate configure snapshot parameter и owned Normal priority для следующего separately reviewed build; old cache не переносится.
- 2026-10-28: после review новый configure и single-worker native build завершились0;14TU/19tasks и3 свежих ELF executables прошли static inspection. Android execution/host traffic не запускались; первый failed attempt сохранён.
