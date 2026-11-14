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

// The engine patch (patches/0004-android-host) provides the definition in
// ps2_android_runtime.cpp.  Declared before its uses below.
extern "C" int ps2x_android_game_main(void);

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
    ps2x::host::Log("I/host", "TEMP-DIAG diag41 padScript setting: %s",
                    padScript != nullptr ? padScript->c_str() : "(absent)");
    SetEnvVar("GOW2_AUDIO_VOLUME", volumeText);
    SetEnvVar("GOW2_PAD_DEADZONE", deadzoneText);
    SetEnvVar("PS2X_LOG_LEVEL", logLevel != nullptr ? *logLevel : std::string("info"));
    SetEnvVar("GOW_VSYNC", SettingBool(settings, "vsync", true) ? std::string("1") : std::string("0"));
    SetEnvVar("GOW2_ISO_URI", isoUri != nullptr ? *isoUri : std::string());
    const std::string* isoPath = FindSetting(settings, "isoPath");
    if (isoPath != nullptr && !isoPath->empty()) {
        SetEnvVar("GOW2_ISO", *isoPath);
    }
    const std::string* bootElf = FindSetting(settings, "bootElf");
    if (bootElf != nullptr && !bootElf->empty()) {
        SetEnvVar("GOW2_BOOT_ELF", *bootElf);
    }
    SetEnvVar("GOW_PERF_DIAG", std::string("1"));
    SetEnvVar("GOW_PERF_FRAME_PC", std::string("0x0014E380"));
    SetEnvVar("GOW_GS_FINISH_ASINCRONO", std::string("1")); // TEMP-DIAG diag69: back to async FINISH (kit default). diag68 proved draw() CPU is only ~0.2 s of gs_ms ~3.6 s/5 s — the rest is fence-wait on GPU completion under sync finish.
    // Diagnostic build (kit patch 0006): IOP traces for the SCE-logo hang investigation
    // (SMPD completion path: LIBSD native calls, IOP->EE SIF DMA, SIF RPC, native stats).
    // GOW_SND_DIAG is intentionally not set here: its read site is a namespace-scope
    // initializer that runs at library load, before this function, so patch 0006
    // defaults it at the read site instead.
    SetEnvVar("PS2X_IOP_TRACE_DMA", std::string("1"));
    SetEnvVar("PS2X_TRACE_RPC", std::string());
    SetEnvVar("PS2X_IOP_NATIVE_STATS", std::string("1"));
    // Diagnostic build 2/3 (kit patches 0007/0008): trace the disc reads that feed
    // the sound bank loader. The sampled IOP import trace from diag2 is dropped
    // again: its volume perturbs IOP/EE timing.
    SetEnvVar("GOW_CD_TRACE", std::string("1"));
    // VU1 diagnostics: the perf line attributes ~95% wall-clock to the VU
    // bucket (guest ~2.2 flips/s). Print run/cycle/interpreter statistics
    // every 5 s so the next device run shows whether native-block coverage
    // or non-terminating microprograms are responsible.
    SetEnvVar("GOW_VU1_STATS", std::string("1"));
    // TEMP-DIAG (guest-state trace; revert after diagnosis): movie-player
    // probes, periodic call profile and thread watch from the GOW2 overrides.
    SetEnvVar("GOW2_MOVIE_TRACE", std::string("1")); // probes stay registered; prints are gated below
    // TEMP-DIAG diag115: MOVIE_TRACE's change-prints flooded the 80 MB log cap
    // in the mode-13 regime (17:49 cap hit hid the present wedge). Watch/mode
    // lines + probe counts (printed by the watch) do not need the per-change
    // prints; the overrides check this env to skip them.
    SetEnvVar("GOW2_MOVIE_TRACE_PRINTS", std::string("0"));
    SetEnvVar("GOW_MPEG_DIAG", std::string("1"));
    SetEnvVar("GOW_MPEG_PICDUMP", std::string("1"));
    SetEnvVar("GOW2_THREAD_WATCH", std::string("5"));
    SetEnvVar("GOW_VBLANK_DIAG", std::string("1"));
    SetEnvVar("GOW_VU1N_DIAG", std::string("1"));
    SetEnvVar("GOW_VU1_BUDGET_DIAG", std::string("1"));
    // TEMP-DIAG diag120: switch the (existing) call profiler on for the
    // mode-13 locomotion census — does the player update actually execute?
    SetEnvVar("GOW2_CALL_PROFILE", std::string("5"));
    // TEMP-DIAG capture build: dump VU1 microcode + IOP module images to
    // /sdcard/gow2/capture so scripts/gen-native can run on this workstation
    // (the desktop runner cannot start here: the CI VM has no OpenGL driver).
    // This workstation CI VM has no OpenGL driver, so the desktop capture run
    // dies before the engine starts; the same capture hooks work on device.
    SetEnvVar("GOW_VU1_CAPTURA", std::string("/sdcard/gow2/capture/vu1"));
    SetEnvVar("PS2X_IOP_DUMP_MODULES", std::string("/sdcard/gow2/capture/iop"));
    SetEnvVar("GOW2_PEEK", std::string("2D7F44,2D7F48,2D8104,2D8130,2D8154,2D81B4,2D8190,2D8108,2D810C,36A6A0,2E9240,2E9244,2E9254,2E925C,2E9280,2E9284,2D8138,2D8194,33E718,33E840,33E844,33E848,33E84C,33E850,33E854,33769C,3376A0,3376CC,3376DC,337700,337588,337584,33F380,33F384,33F388,33F38C,33F390,33F394,33F398,33F39C,33F3A0,33F3A4,33F3C8,33F3CC,33F3D0,33F3D4,33F3D8,33F3DC,33F3E0,33F3E4,33F3F8,33F404,33F408,33F578,33F57C,33F588,33F58C,33F5AC,33F5B0,33F5B4,33F5B8,33F5BC,33F5C0,33F5C4,33F5D8,33F5DC,33F5E0,369068,36906C,369070,369074,369078,369080,369084,369088,36908C,369090,369094,2D8110,2D8114,2D8118,369048,369058,32F338,32F33C,32F358,32F35C,2D8EC0"));
    // Native VU1/IOP code is generated from on-device captures
    // (GOW_VU1_CAPTURA / PS2X_IOP_DUMP_MODULES) by scripts/gen-native.sh and
    // compiled in via ps2xNativeVu1Dir; see generated/capture/. The capture
    // envs are intentionally NOT set in production builds.
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
