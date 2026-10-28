# Changelog

## Unreleased

- Proxy и Core используют общий bounded fixed-name selector собственной сети.
- Experimental proxy проверяет public ABI собственной DLL, не вызывая и не
  подделывая private stock post-create service. Stock dispatch сохранён.
- Добавлены Win32 environment/bootstrap contract и actual proxy compile target;
  игровой runtime остаётся not_run.
