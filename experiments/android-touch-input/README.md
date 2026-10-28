# Android semantic touch input (U1 source preparation)

Portable reducer + настоящий NDK `AInputEvent` adapter. Не загружает `libGame.so`, не содержит engine ABI/offsets, MTA hooks, UI/overlay или generated game input. Actual game/U1 остаётся `not_run`.

Reuse search перед реализацией: в `Client/Shared/experiments` не найден `AInputEvent`/`AMotionEvent_getPointerId` backend. `Client/sdk/game/CPad.h` задаёт PC controller representation; `CClientPad.cpp` именует действия, но обслуживает scripted peds. Их memory layout не копируется в Android engine.

Host задаёт `InputLayout`: input surface width/height и safe-area rectangle в **тех же pixel coordinates**, что получает native motion event; три непересекающиеся зоны `[0,1]` внутри safe area. Insets/cutout и transform при viewport/surface changes предоставляет host. Это инъекция геометрии, не готовый UI. Invalid/overlapping layout отключает gameplay и сбрасывает state. Граница zone справа/снизу исключена; палец захватывается только на down внутри safe-area/зоны, затем владеет ролью до up/cancel. Дополнительные пальцы не перехватывают занятый control.

Movement: floating anchor на down; radial deadzone/rescale, vector magnitude <=1, X right/Y down. Radius — fraction меньшей safe-area dimension. Camera: накопленный delta / safe width,height (не абсолютный yaw/pitch), ограничен ±8 screen lengths/axis/frame. `ConsumeFrame()` забирает camera delta **один раз**, held movement/action сохраняет. MTA controls/aim/fire/drive mapping и camera sensitivity — следующая интеграция; текущий action — один нейтральный held control, не все MTA actions. Map может переключать injected zones/context для on-foot/vehicle; переключение через Configure сбрасывает старый контекст.

`Apply()` получает полную pointer snapshot; владение хранится по ID, никогда по index. `PointerUp/End` snapshot включает поднимаемый палец и его последнюю позицию. Native adapter читает `AMotionEvent_getPointerId`; encoded action index используется только для выбора changed ID. MOVE применяет каждый ID независимо от index reorder. Touchscreen motion обрабатывается; mouse/joystick/key/hover остаются host. History samples не читаются (coalesced current position); velocity/gesture recognition не заявлены. Missing up лечится отсутствием ID в следующем полном snapshot; invalid IDs/duplicates/NaN/слишком большой count очищают held и delta. Новый Begin освобождает старые owners, сохраняя ещё не потреблённый delta завершённого gesture того же контекста.

Однопоточный контракт: native event и ConsumeFrame выполняются на одном owner thread либо host сериализует их. Никаких atomic/thread-safety обещаний. NDK event borrowed только до возврата; host сохраняет ответственность `AInputQueue_finishEvent` и очередь UI. На `APP_CMD_LOST_FOCUS` вызвать SetFocused(false), pause — SetPaused(true), resize/rotation/safe-area change — Configure; resume/focus gained вернуть соответствующие flags после готовности surface. Нельзя подать старую viewport geometry после rotation. Показываемый IME/chat — SetImeVisible(true), закрытие — false. Android back/UI modal переход — Reset и соответствующая host focus policy; key events/IME composition этим adapter не реализуются. Все transitions очищают owners, held state, pending delta и повышают epoch. Старый MOVE после resume не reacquires controls; нужен новый down. UI, забравший gesture, обязан cancel gameplay state, а не скрыть от него только up.

Portable tests содержат реальные reducer traces: movement+camera+third action, index reorder до/после up, one-shot camera delta, deadzone и unit diagonal clamp, safe insets, rotation, missing up, cancel, focus/pause/IME, malformed IDs/NaN/bounds. Это не Android OS event-delivery test.

Standalone commands для разрешённого build window (один worker; dependencies отсутствуют):

```powershell
cmake -S experiments/android-touch-input -B '<ignored lab>/touch-native' -G Ninja
cmake --build '<ignored lab>/touch-native' --parallel 1
ctest --test-dir '<ignored lab>/touch-native' --output-on-failure
cmake -S experiments/android-touch-input -B '<ignored lab>/touch-android-arm64' -G Ninja -DCMAKE_TOOLCHAIN_FILE='<existing NDK>/build/cmake/android.toolchain.cmake' -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static
cmake --build '<ignored lab>/touch-android-arm64' --parallel 1
```

Host tests не компилируют Android adapter. NDK build компилирует его с настоящим `<android/input.h>` и link dependency `android`; запуск ELF/AVD и доставка реальных MotionEvents — отдельные проверки. До actual evidence нельзя объявлять даже эти compile/test commands выполненными. Полный U1 требует movement+look/aim и drive traces уже в игре, UI scale/safe-area проверки, Android lifecycle/surface recovery и отсутствие stuck controls.

Actual `touch-002`, source `7be7b6ac606ae5db9ecfec86ef898f450a8702db`: Win32 MSVC19.51 compile + portable CTest 1/1 PASS (0.38s); NDK28.2.13676358 ARM64/API28 singleworker build PASS. Captures, commands/PID/exit/guards, source/artifact SHA256 и ELF headers — в `verification/touch-002/`. Это compile proof native adapter **в static archive**; его native imports пока не проверены финальным executable link. Android test ELF не запускался, реальных AInputEvents/game/U1 runtime не было. Binary artifacts остаются в ignored lab.
