# A2: exact GameThread frame bridge

Own source slice для конкретного APK2.11.311 SHA
`348750d29d2e463768a2c806dc01c331f2b88fce1646fba8e7485e6c3ff6ff50`.
Оригинальный APK не изменён; private patched DEX/APK ещё не создавался, game/JNI
runtime не выполнялся. Source refs используют [проверенный launch map](../android-launch-entry-map/README.md)
и сохранённые ignored captures, без копирования чужих class bodies в Git.

## Реальный seam

REA skill прочитан. Existing `entrypoints/executor.stdout.log` SHA
`10d300034fb494aedbe8f535ab2dce3bd0b9f783718ef9f15d9a14538536d35d`
показывает private `ExecutorThread.guardedRun()V`, registers7, единственный
`invoke-static {v4}, GameNative.implOnDrawFrame(F)V` после queue drain и
реальных `hasFocus && implIsInitialized` условий. Own transformer изменяет только
MethodReference этой инструкции на `org.mta.mobile.frame.FrameBridge.draw(F)V`.
Opcode/registers/width3codeunits остаются прежними; branch targets, try ranges,
debug items и class/method metadata не редактируются.

Оригинальная public static native declaration `GameNative.implOnDrawFrame(F)V`
обязана остаться без implementation. Cached reflection в own shim проверяет
именно эти modifiers/descriptor и вызывает оригинальный native ровно один раз.
`InvocationTargetException` разворачивается до исходного Throwable; finally
записывает completed/failed, не заменяя исходное исключение своим JNI-сбоем.
`GameNative` class initialization остаётся original loadLibrary("Game"); в seam
до own shim уже выполнен implIsInitialized. Own lib загружается после этого,
ошибка загрузки отключает только telemetry с явным Log.e, draw сохраняется.

Java привязывает telemetry к первому Thread object, native — к его actual gettid
и Java thread ID. Before возвращает monotonic ticket, after требует тот же tid и
ticket. Reentry/mismatch/invalid delta отключает telemetry. Logging ограничен
первыми3frames и каждым300-м, failure выводится отдельно. Epoch1 означает только
process-local attachment: pause/surface/teardown и lifetime **не доказаны**.
Существующие lifecycle native invokes не патчатся этим slice; sampler/GOT чтение
и sensor controls здесь не вызываются, networking/Lua ACK отсутствуют.

## Mature DEX tooling и audit

Новых инструментов не установлено. На машине уже имеются official Android SDK:

- `C:/Android/cmdline-tools/latest/lib/external/com/android/tools/smali/smali-dexlib2/3.0.9/smali-dexlib2-3.0.9.jar`;
- smali-baksmali/util3.0.9, Guava33.4.0 и SDK apkanalyzer classpath;
- JDK21, D8 и apksigner36.0.0.

APKTool/smali assembler CLI в проверенных PATH/D:/bin/SDK не найден. Он не нужен
для выбранного writer API: `PatchFrameInvoke.java` использует official dexlib2
DexRewriter/ImmutableInstruction35c/DexPool, не собственный DEX encoder.
Pinned SHA разрешены только для исходных classes.dex/classes2.dex/classes3.dex.
Tool требует exact owner/method/descriptor/registers, one call и неизменённую
native declaration, затем повторно читает patched DEX через dexlib2.

Restore audit официальным writer возвращает единственную ссылку назад и
сравнивает canonical original/restored DEX byte-for-byte. Это проверка semantic
roundtrip всех writer-представимых classes/instructions/metadata, не обещание
сохранить неизвестные vendor DEX sections или оригинальный byte layout. Source
не перезаписывается; audit directory fresh-only, сбойные результаты сохраняются.
Own helper classes должны попасть в отдельный новый DEX через D8; этот tool
сам их не добавляет и не подписывает APK.

Перед local APK output нужны independent source review, actual tool compile и
roundtrip, проверка own DEX descriptors/двух JNI exports, manifest extraction
policy, exact изменяемые ZIP members, original/output hashes и новый local
signer identity. Никаких signature/service/security bypass. Original signing
identity при локальной переподписи не сохраняется. Game binary/DEX bodies и
repacked APK остаются в ignored lab, в репозиторий идут own source/metadata.

## Ограничения

Reflection добавляет boxing/dispatch overhead; это диагностический прототип,
не production hot path. Thread/ticket binds не являются доказательством engine
ownership, physics phase или CPed lifetime. Реальный first-frame log, loading,
pause/resume/teardown и actual game drawing ещё предстоит проверить. Успешная
компиляция собственных классов не доказывает, что modified APK запускается.

## Actual javac002: 2026-10-30

Frozen source `cd9d9ca829195568f12ee3aeca3d60840117cb6d`: actual javac helper
и dexlib2 tool завершились exit0, Java class output hashes сохранены в
[receipt](verification/javac-windows-2026-10-30.json). JDK21 напечатал по3warnings
о deprecated source/target8 options, ошибок компиляции нет. Raw logs/driver в
ignored `D:/projects/astana/work/mta-mobile-lab/frame-bridge-javac-002`.
Run001 до этого отказал до compiler при1297MiB/пороге1536; после отдельного
root разрешения tiny javac-only002 выполнен с floor768MiB, heap256MiB,
metaspace128MiB и одним CPU, при фактических1169/1247MiB RAM и отсутствииVM.
Эта policy не применяется к native/NDK/VM. Original run001 не изменялся.

Воспроизводимые команды с новыми output dirs после отдельного resource/process
preflight (JDKиSDK уже установлены):

```powershell
& 'C:/Program Files/Java/jdk-21.0.11/bin/javac.exe' `
  -J-Xmx256m -J-XX:MaxMetaspaceSize=128m -J-XX:ActiveProcessorCount=1 `
  --release 8 -cp C:/Android/platforms/android-34/android.jar `
  -d <fresh-helper-classes> experiments/android-frame-bridge/java/org/mta/mobile/frame/FrameBridge.java
& 'C:/Program Files/Java/jdk-21.0.11/bin/javac.exe' `
  -J-Xmx256m -J-XX:MaxMetaspaceSize=128m -J-XX:ActiveProcessorCount=1 `
  --release 8 -cp C:/Android/cmdline-tools/latest/lib/apkanalyzer-classpath.jar `
  -d <fresh-tool-classes> experiments/android-frame-bridge/tools/PatchFrameInvoke.java
```

DEX transformer ещё не исполнялся; canonical roundtrip в source — требуемая
проверка будущего запуска, не уже полученный PASS. Native bridge, helper D8,
APK packaging/signing/install и actual frame delivery пока not_run.
