// JNI bridge between com.gow2.android.NativeBridge and the host.
//
// No C++ exception ever crosses the JNI boundary: every export has a try/catch
// that degrades to a log line (or a safe default) instead of unwinding into ART.
#include "ps2x_host.h"

#include "ps2x_host_internal.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>
#include <signal.h>

// The engine patch (Lead) provides the strong definition; game_entry.cpp carries
// the weak fallback.  Declared before its uses below.
extern "C" int ps2x_android_game_main(void) __attribute__((weak));

namespace {

constexpr const char* kBridgeClass = "com/gow2/android/NativeBridge";

std::atomic<JavaVM*> g_vm{nullptr};
std::atomic<jclass> g_bridgeClass{nullptr};
std::atomic<jmethodID> g_rumbleMethod{nullptr};

std::mutex g_sessionMutex;
std::thread g_gameThread;
bool g_started = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string ToUtf8(JNIEnv* env, jstring value)
{
    if (env == nullptr || value == nullptr)
        return std::string();
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr)
        return std::string();
    std::string copy(chars);
    env->ReleaseStringUTFChars(value, chars);
    return copy;
}

jstring NewString(JNIEnv* env, const std::string& value)
{
    if (env == nullptr)
        return nullptr;
    return env->NewStringUTF(value.c_str());
}

void SetEnvVar(const char* name, const std::string& value)
{
    if (name == nullptr)
        return;
    const int rc = ::setenv(name, value.c_str(), 1);
    ps2x::host::Log("host/env", "%s=%s (%s)", name, value.c_str(), rc == 0 ? "ok" : "failed");
}

void CacheBridgeClass(JNIEnv* env)
{
    if (env == nullptr || g_bridgeClass.load(std::memory_order_acquire) != nullptr)
        return;
    jclass local = env->FindClass(kBridgeClass);
    if (local == nullptr) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        ps2x::host::Log("E/host/jni", "NativeBridge class not found");
        return;
    }
    auto global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_bridgeClass.store(global, std::memory_order_release);
}

int ReadSdkInt(JNIEnv* env)
{
    if (env == nullptr)
        return 0;
    jclass version = env->FindClass("android/os/Build$VERSION");
    if (version == nullptr) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return 0;
    }
    const jfieldID field = env->GetStaticFieldID(version, "SDK_INT", "I");
    int sdk = 0;
    if (field != nullptr)
        sdk = env->GetStaticIntField(version, field);
    env->DeleteLocalRef(version);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    return sdk;
}

std::string JsonEscape(const char* text)
{
    std::string out;
    if (text == nullptr)
        return out;
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof escape, "\\u%04x", c);
                out += escape;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Settings JSON: flat object only, no third-party parser.
// ---------------------------------------------------------------------------
using SettingsList = std::vector<std::pair<std::string, std::string>>;

void SkipSpace(const char*& cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
}

std::string ParseString(const char*& cursor)
{
    std::string out;
    if (*cursor != '"')
        return out;
    ++cursor;
    while (*cursor != '\0' && *cursor != '"') {
        char c = *cursor++;
        if (c != '\\') {
            out += c;
            continue;
        }
        const char escape = *cursor++;
        switch (escape) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            // Settings strings are ASCII; consume the escape and move on.
            for (int i = 0; i < 4 && cursor[i] != '\0'; ++i)
                ++cursor;
            out += '?';
            break;
        }
        default:
            out += escape;
            break;
        }
    }
    if (*cursor == '"')
        ++cursor;
    return out;
}

bool ParseFlatJson(const char* json, SettingsList& out)
{
    if (json == nullptr)
        return false;
    const char* cursor = json;
    SkipSpace(cursor);
    if (*cursor != '{')
        return false;
    ++cursor;
    for (;;) {
        SkipSpace(cursor);
        if (*cursor == '}') {
            ++cursor;
            return true;
        }
        if (*cursor == '\0')
            return false;
        const std::string key = ParseString(cursor);
        SkipSpace(cursor);
        if (*cursor != ':')
            return false;
        ++cursor;
        SkipSpace(cursor);

        std::string value;
        if (*cursor == '"') {
            value = ParseString(cursor);
        } else {
            const char* start = cursor;
            while (*cursor != '\0' && *cursor != ',' && *cursor != '}')
                ++cursor;
            const char* end = cursor;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
                --end;
            value.assign(start, static_cast<size_t>(end - start));
        }
        if (!key.empty())
            out.emplace_back(key, value);

        SkipSpace(cursor);
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor == '}') {
            ++cursor;
            return true;
        }
        if (*cursor == '\0')
            return false;
    }
}

const std::string* FindSetting(const SettingsList& settings, const char* key)
{
    for (const auto& entry : settings) {
        if (entry.first == key)
            return &entry.second;
    }
    return nullptr;
}

bool SettingBool(const SettingsList& settings, const char* key, bool fallback)
{
    const std::string* value = FindSetting(settings, key);
    if (value == nullptr)
        return fallback;
    return *value == "true" || *value == "1";
}

float SettingFloat(const SettingsList& settings, const char* key, float fallback)
{
    const std::string* value = FindSetting(settings, key);
    if (value == nullptr || value->empty())
        return fallback;
    return static_cast<float>(std::atof(value->c_str()));
}

float PresetScale(const std::string& preset)
{
    if (preset == "performance")
        return 0.75f;
    if (preset == "quality")
        return 1.5f;
    return 1.0f; // balanced
}

std::string AspectEnvValue(const std::string& aspect)
{
    // fill -> 16:9, 4:3 -> 4:3, fit -> 16:9 (letterboxed by the backend).
    if (aspect == "4:3")
        return std::string("4:3");
    if (aspect == "fit")
        return std::string("16:9");
    return std::string("16:9");
}

void ApplySettingsJson(const char* json)
{
    SettingsList settings;
    if (!ParseFlatJson(json, settings)) {
        ps2x::host::Log("E/host", "settings JSON could not be parsed; using defaults");
    }

    const std::string* preset = FindSetting(settings, "preset");
    float scale = SettingFloat(settings, "scale", -1.0f);
    if (scale <= 0.0f)
        scale = PresetScale(preset != nullptr ? *preset : std::string("balanced"));

    char scaleText[32];
    std::snprintf(scaleText, sizeof scaleText, "%.3f", static_cast<double>(scale));

    const std::string* aspect = FindSetting(settings, "aspect");
    const std::string* padScript = FindSetting(settings, "padScript");
    const std::string* logLevel = FindSetting(settings, "logLevel");
    const std::string* isoUri = FindSetting(settings, "isoUri");

    char volumeText[32];
    std::snprintf(volumeText, sizeof volumeText, "%.3f",
        static_cast<double>(SettingFloat(settings, "volume", 0.8f)));

    char deadzoneText[32];
    std::snprintf(deadzoneText, sizeof deadzoneText, "%.3f",
        static_cast<double>(SettingFloat(settings, "deadzone", 0.18f)));

    SetEnvVar("PS2X_GS_HW_SCALE", scaleText);
    SetEnvVar("GOW_ASPECT", AspectEnvValue(aspect != nullptr ? *aspect : std::string("fill")));
    SetEnvVar("GOW2_PAD_SCRIPT", padScript != nullptr ? *padScript : std::string());
    SetEnvVar("GOW2_AUDIO_VOLUME", volumeText);
    SetEnvVar("GOW2_PAD_DEADZONE", deadzoneText);
    SetEnvVar("PS2X_LOG_LEVEL", logLevel != nullptr ? *logLevel : std::string("info"));
    SetEnvVar("GOW_VSYNC", SettingBool(settings, "vsync", true) ? std::string("1") : std::string("0"));
    SetEnvVar("GOW2_ISO_URI", isoUri != nullptr ? *isoUri : std::string());
    SetEnvVar("GOW_PERF_DIAG", std::string("1"));
}

void GameThreadMain()
{
    ::pthread_setname_np(::pthread_self(), "ps2x-game");
    ps2x::host::Log("host", "game thread started");
    const int result = ps2x_android_game_main();
    ps2x::host::Log("host", "game thread returned %d", result);
}

} // namespace

// ---------------------------------------------------------------------------
// JNI lifecycle
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    g_vm.store(vm, std::memory_order_release);
    JNIEnv* env = nullptr;
    if (vm != nullptr && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        CacheBridgeClass(env);
    ps2x::host::detail::CrashInstall();
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* /*reserved*/)
{
    ps2x::host::Log("host", "JNI_OnUnload");
    g_vm.store(nullptr, std::memory_order_release);
    jclass bridge = g_bridgeClass.exchange(nullptr, std::memory_order_acq_rel);
    if (bridge != nullptr) {
        JNIEnv* env = nullptr;
        if (vm != nullptr && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
            env->DeleteGlobalRef(bridge);
    }
}

namespace ps2x::host::detail {

void RumbleToJava(float low, float high)
{
    try {
        JavaVM* vm = g_vm.load(std::memory_order_acquire);
        jclass bridge = g_bridgeClass.load(std::memory_order_acquire);
        if (vm == nullptr || bridge == nullptr)
            return;

        JNIEnv* env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK)
                return;
            attached = true;
        }

        jmethodID method = g_rumbleMethod.load(std::memory_order_acquire);
        if (method == nullptr) {
            method = env->GetStaticMethodID(bridge, "dispatchRumble", "(FF)V");
            if (method == nullptr) {
                if (env->ExceptionCheck())
                    env->ExceptionClear();
                if (attached)
                    vm->DetachCurrentThread();
                return;
            }
            g_rumbleMethod.store(method, std::memory_order_release);
        }

        env->CallStaticVoidMethod(bridge, method, static_cast<jfloat>(low), static_cast<jfloat>(high));
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (attached)
            vm->DetachCurrentThread();
    } catch (...) {
        // Rumble is cosmetic; never let it take the game down.
    }
}

} // namespace ps2x::host::detail

// ---------------------------------------------------------------------------
// Exports used by com.gow2.android.NativeBridge
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeStart(JNIEnv* env, jclass, jstring filesDir,
    jstring settingsJson)
{
    try {
        if (g_vm.load(std::memory_order_acquire) == nullptr && env != nullptr) {
            JavaVM* vm = nullptr;
            if (env->GetJavaVM(&vm) == JNI_OK)
                g_vm.store(vm, std::memory_order_release);
        }
        CacheBridgeClass(env);

        const std::string files = ToUtf8(env, filesDir);
        const std::string settings = ToUtf8(env, settingsJson);
        if (files.empty()) {
            ps2x::host::Log("E/host/jni", "nativeStart without a files directory");
            return;
        }

        std::lock_guard<std::mutex> lock(g_sessionMutex);
        if (g_started) {
            ps2x::host::Log("host", "nativeStart ignored: session already running");
            return;
        }

        ps2x::host::detail::ResetSession();
        ps2x::host::detail::SetFilesDir(files.c_str());
        ps2x::host::StartLog(files.c_str());

        const int sdk = ReadSdkInt(env);
        ps2x::host::SetSdkInt(sdk);

        SetEnvVar("GOW2_BOOT_ELF", files + "/SCUS_974.81");
        SetEnvVar("GOW2_ISO", files + "/gow2.iso");
        ApplySettingsJson(settings.c_str());

        g_started = true;
        g_gameThread = std::thread(&GameThreadMain);
    } catch (...) {
        ps2x::host::Log("E/host/jni", "nativeStart failed with a C++ exception");
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeStop(JNIEnv* /*env*/, jclass)
{
    try {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        if (!g_started)
            return;
        ps2x::host::Log("host", "nativeStop: asking the engine to exit");
        ps2x::host::RequestExit();
        ps2x::host::detail::RequestShutdown();
        ps2x::host::StopAudio();
        if (g_gameThread.joinable())
            g_gameThread.join();
        ps2x::host::AttachWindow(nullptr);
        g_started = false;
        ps2x::host::Log("host", "nativeStop: engine thread joined");
    } catch (...) {
        ps2x::host::Log("E/host/jni", "nativeStop failed with a C++ exception");
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeSetSurface(JNIEnv* env, jclass, jobject surface)
{
    try {
        ANativeWindow* window = nullptr;
        if (env != nullptr && surface != nullptr)
            window = ANativeWindow_fromSurface(env, surface);
        ps2x::host::AttachWindow(window);
        if (window != nullptr)
            ANativeWindow_release(window);
    } catch (...) {
        ps2x::host::Log("E/host/jni", "nativeSetSurface failed");
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeSurfaceChanged(JNIEnv* /*env*/, jclass, jint width,
    jint height)
{
    ps2x::host::SetSurfaceSize(static_cast<int>(width), static_cast<int>(height));
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeSetPadState(JNIEnv* /*env*/, jclass, jint buttons,
    jint lx, jint ly, jint rx, jint ry)
{
    ps2x::host::SetPadState(static_cast<uint16_t>(buttons & 0xFFFF), static_cast<int8_t>(lx), static_cast<int8_t>(ly),
        static_cast<int8_t>(rx), static_cast<int8_t>(ry));
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeSetPaused(JNIEnv* /*env*/, jclass, jboolean paused)
{
    ps2x::host::SetPaused(paused == JNI_TRUE);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_gow2_android_NativeBridge_nativeLogPath(JNIEnv* env, jclass)
{
    return NewString(env, ps2x::host::LogFilePath());
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeRequestExit(JNIEnv* /*env*/, jclass)
{
    ps2x::host::RequestExit();
}

extern "C" JNIEXPORT jstring JNICALL Java_com_gow2_android_NativeBridge_nativeHostStats(JNIEnv* env, jclass)
{
    try {
        int width = 0;
        int height = 0;
        ps2x::host::GetSurfaceSize(width, height);
        uint16_t buttons = 0;
        int8_t lx = 0;
        int8_t ly = 0;
        int8_t rx = 0;
        int8_t ry = 0;
        ps2x::host::GetPadState(buttons, lx, ly, rx, ry);

        char text[256];
        std::snprintf(text, sizeof text,
            "tick=%llu audio=%llu ready=%d surface=%dx%d paused=%d exit=%d pad=%04x %d/%d %d/%d",
            static_cast<unsigned long long>(ps2x::host::VsyncTick()),
            static_cast<unsigned long long>(ps2x::host::AudioFramesPlayed()),
            ps2x::host::IsAudioReady() ? 1 : 0, width, height, ps2x::host::IsPaused() ? 1 : 0,
            ps2x::host::ShouldExit() ? 1 : 0, static_cast<unsigned>(buttons), static_cast<int>(lx), static_cast<int>(ly),
            static_cast<int>(rx), static_cast<int>(ry));
        return NewString(env, std::string(text));
    } catch (...) {
        return NewString(env, std::string("stats unavailable"));
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_com_gow2_android_NativeBridge_nativeExtractDisc(JNIEnv* env, jclass,
    jstring isoPath, jstring filesDir)
{
    try {
        const std::string iso = ToUtf8(env, isoPath);
        const std::string files = ToUtf8(env, filesDir);
        if (!files.empty()) {
            ps2x::host::detail::SetFilesDir(files.c_str());
            if (ps2x::host::LogFilePath().empty())
                ps2x::host::StartLog(files.c_str());
        }

        char error[256] = {0};
        const bool ok = ps2x::host::ExtractDiscFromIso(iso.c_str(), error, sizeof error);
        const auto& info = ps2x::host::detail::LastDiscExtract();

        std::string json;
        if (ok) {
            json = "{\"ok\":true,\"elf\":\"" + JsonEscape(info.elfName) + "\",\"sha1\":\"" + JsonEscape(info.elfSha1) +
                   "\",\"match\":" + (info.sha1Matches ? "true" : "false") + "}";
        } else {
            const char* message = info.error[0] != '\0' ? info.error : error;
            json = "{\"ok\":false,\"error\":\"" + JsonEscape(message) + "\"}";
        }
        return NewString(env, json);
    } catch (...) {
        return NewString(env, std::string("{\"ok\":false,\"error\":\"native exception\"}"));
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_gow2_android_NativeBridge_nativeCrashTest(JNIEnv* /*env*/, jclass)
{
    // Writes a full report and then raises SIGABRT so the signal handler path is
    // exercised end to end.  Hidden behind a long-press in the launcher UI.
    ps2x::host::Log("E/host", "nativeCrashTest: raising SIGABRT on purpose");
    ps2x::host::detail::CrashWriteReport(0, "crash test requested from Java");
    ::raise(SIGABRT);
}
