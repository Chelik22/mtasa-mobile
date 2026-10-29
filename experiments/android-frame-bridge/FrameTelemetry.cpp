#include <android/log.h>
#include <jni.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sys/types.h>
#include <unistd.h>

namespace
{
    struct State
    {
        std::mutex    mutex;
        pid_t         ownerTid{};
        jlong         javaThreadId{};
        std::uint64_t epoch{};
        jlong         sequence{};
        jlong         pending{};
        bool          disabled{};
    };
    State state;

    void Disable(const char* reason)
    {
        state.disabled = true;
        __android_log_print(ANDROID_LOG_ERROR, "MtaFrameBridge", "{\"event\":\"disabled\",\"reason\":\"%s\",\"pid\":%d,\"tid\":%d}", reason, getpid(),
                            gettid());
    }
}

extern "C" JNIEXPORT jlong JNICALL Java_org_mta_mobile_frame_FrameBridge_nativeBefore(JNIEnv*, jclass, jlong javaThreadId, jfloat delta)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.disabled)
        return 0;
    if (!state.ownerTid)
    {
        state.ownerTid = gettid();
        state.javaThreadId = javaThreadId;
        // This is a process-local attachment epoch, not a proven lifecycle epoch.
        state.epoch = 1;
    }
    if (state.ownerTid != gettid() || state.javaThreadId != javaThreadId || javaThreadId <= 0 || state.pending ||
        state.sequence == std::numeric_limits<jlong>::max() || !std::isfinite(delta) || delta < 0)
    {
        Disable("thread_or_sequence_or_delta_mismatch");
        return 0;
    }
    state.pending = ++state.sequence;
    if (state.sequence <= 3 || state.sequence % 300 == 0)
        __android_log_print(ANDROID_LOG_INFO, "MtaFrameBridge",
                            "{\"event\":\"before_original_draw\",\"pid\":%d,\"tid\":%d,\"java_thread\":%lld,\"epoch\":%llu,\"sequence\":%lld,\"delta\":%.9g}",
                            getpid(), state.ownerTid, static_cast<long long>(javaThreadId), static_cast<unsigned long long>(state.epoch),
                            static_cast<long long>(state.sequence), static_cast<double>(delta));
    return state.pending;
}

extern "C" JNIEXPORT void JNICALL Java_org_mta_mobile_frame_FrameBridge_nativeAfter(JNIEnv*, jclass, jlong ticket, jboolean completed)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.disabled)
        return;
    if (state.ownerTid != gettid() || ticket <= 0 || state.pending != ticket)
    {
        Disable("after_thread_or_ticket_mismatch");
        return;
    }
    state.pending = 0;
    if (ticket <= 3 || ticket % 300 == 0 || !completed)
        __android_log_print(ANDROID_LOG_INFO, "MtaFrameBridge",
                            "{\"event\":\"after_original_draw\",\"pid\":%d,\"tid\":%d,\"epoch\":%llu,\"sequence\":%lld,\"completed\":%s}", getpid(),
                            state.ownerTid, static_cast<unsigned long long>(state.epoch), static_cast<long long>(ticket), completed ? "true" : "false");
}
