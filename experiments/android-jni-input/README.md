# Own Java MotionEvent → JNI → semantic input

Подготовлен source slice. Compile, JVM/JNI trace, DEX, Android event delivery, game и U1 runtime — **not_run** на этом commit. APK не менялся/не упаковывался/не устанавливался. Game/native engine functions не вызываются; GameView/GameNative bodies не копируются.

A2 map `17eefb1de` установил фактический путь: GameView (SurfaceView под AppCompatActivity) → InputHandler → GameThread scalar callbacks → JNI; ExecutorThread исполняет очередь перед implOnDrawFrame. Установленный NDK предоставляет AMotionEvent_fromJava только с API31. Этот helper работает через Java MotionEvent APIs для minSdk28 и переиспользует AndroidInputState без дублирования reducer и без layouts libGame.

`MotionEventInput.java` получает borrowed MotionEvent синхронно на UI-thread seam. Владение — по pointer ID; action index выбирает только changed ID. Полный snapshot включает поднимаемый палец. CANCEL/OUTSIDE сбрасывает input даже при count0; mouse/gamepad/hover игнорируются. Max32, reusable arrays, без сохранения event/View/Activity/Surface. Возврат означает приём snapshot нашим reducer, **не** stock UI consumption: первый observer hook сохраняет существующий dispatch/result и не отправляет второй набор touches в game JNI.

`NativeInputBridge.java` загружает собственную library и владеет context; close явный/idempotent. Начальное состояние unfocused/paused/unconfigured. Bootstrap передаёт фактический focus, viewport и resume/pause. Helper имеет методы focus/pause/IME/back/UI capture/surface loss/viewport; host должен их подключить. IME/Insets нельзя вывести только из touch. Потеря Surface инвалидирует layout до новых размеров.

Layout float[20]: width,height; safe area,movement zone,look zone,action zone (left,top,right,bottom по4); radius,deadzone. Геометрия в pixels того же origin, что MotionEvent; зоны normalized внутри safe area. Неправильная конфигурация отключает старую geometry.

`InputBridge.cpp`: семь настоящих JNI exports, проверка count/array lengths, stack copy до reducer lock. Registry max16 с monotonic positive IDs вместо casts native pointer. Lookup сохраняет shared ownership, затем отпускает registry mutex: уже начатый call не использует освобождённый context при close. Отдельный mutex сериализует UI feed и единственного GameThread consumer. Под locks выполняются только bounded reducer операции: без JNI array access, Java callbacks, engine calls или IO. Нет растущей очереди/JVM global refs. C++ exceptions через JNI не выходят. Malformed snapshots очищают удерживаемые controls; failed/closed Java consume очищает reusable frame.

Frame содержит moveX/Y, одноразовый look deltaX/Y, action, enabled, epoch, sequence. Предлагаемый consumer seam — перед native frame на GameThread. Значения пока не применяются к CPad/aim/drive/camera. Нужен один consumer, учитывающий enabled и результат consume. Нормализация/deadzone/reset portable reducer сохранены.

## Подготовленная проверка, ещё не выполнена

CMake собирает только собственную JNI library + существующий portable source. Для host нужны существующий x64 JDK21 JNI headers и x64 library; NDK использует свой jni.h. По координации root полный PC build приоритетен: javac/NDK/build здесь не запускались.

После получения build window с memory guard, один worker, outputs только ignored lab:

1. CMake Win64 `-G "Visual Studio 18 2026" -A x64`; process-only JAVA_HOME `C:/Program Files/Java/jdk-21.0.11`. Target mta_input_bridge, `/m:1 /nr:false /p:MultiProcessorCompilation=false`.
2. Existing javac `-J-Xmx256m --release 8 -d <classes>` для NativeInputBridge и tests/BridgeJvmTrace.java. Existing java `-Xmx256m -Djava.library.path=<DLL dir> -cp <classes> org.mta.mobile.input.BridgeJvmTrace`. Это настоящий JVM/native JNI roundtrip со scalar snapshots, не Android delivery. Traces: три пальца/reorder/one-shot delta/lifecycle/arrays/duplicate IDs/NaN/geometry/close/context bound.
3. MotionEventInput компилируется с existing `C:/Android/platforms/android-34/android.jar`, javac `-J-Xmx256m --release 8 -cp <android.jar>`. Existing D8 `C:/Android/build-tools/36.0.0/lib/d8.jar`: java -Xmx256m, com.android.tools.r8.D8, min-api28, lib android.jar. Выбрать только production class files: NativeInputBridge, nested Frame, MotionEventInput; JVM trace не упаковывать. Keep rules нужны, если позднее появится R8: JNI names должны сохраниться.
4. Existing NDK28.2.13676358, arm64-v8a/android-28/c++_static, parallel1. Проверить ELF/JNI exports/NEEDED/hashes. Это crosscompile/link, без APK install и запуска ELF.

Сохранить source SHA, raw/LF inputs, команды/PID/logs/exits/tool paths, native/DEX hashes. Source review не превращает эти команды в passed. Actual framework MotionEvent trace и gameplay — последующие runtime checks.

## Конкретный scope следующего packaging review

Собственные файлы: InputBridge.cpp, два Java helpers, keep rules и CMake target. Исходный engine не меняется. До packing сохранить original APK `348750d2…ff50`, libGame `4c6a7445…282e`, три DEX identities из A2; затем actual hashes compiled helper DEX/native library, точный список изменённых ZIP entries и новый package/signature fingerprint.

Предлагаемые будущие hooks: GameView.onTouchEvent (snapshot перед handler), view/activity lifecycle, ExecutorThread перед native frame (один consume). Сам bytecode hook здесь не реализован. Декомпилированные bodies не переносить в Git, services/initialization не подменять успешными заглушками. Root проверит конкретный patch/package до pack/install; перед GUI предупредить пользователя согласно его просьбе о ноутбуке.
