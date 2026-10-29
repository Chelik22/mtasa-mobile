# Changelog

## Unreleased

- Independent review: own before/after/logging Throwable перехватывается только
  на instrumentation boundary, чтобы SecurityException/Error не отменял
  оригинальный draw или его исходное исключение.

- Добавлены own GameThread frame shim, JNI thread/ticket telemetry и focused
  dexlib2 invoke-reference transformer с restore roundtrip audit. APK/runtime
  пока не изменены, lifecycle/sampler capability не включены.
