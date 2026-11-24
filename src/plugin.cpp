
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <algorithm>
#include <cwchar>
#include "scssdk_telemetry.h"
#include "dx11_hook.h"
#include "resource_ids.h"

static scs_log_t g_log = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_stopEvent = nullptr;

static std::atomic_bool g_run{false};
static std::atomic_bool g_paused{true};
static std::atomic_bool g_resetRequested{false};
static std::atomic_bool g_healthWriteFailedLogged{false};

static std::atomic<float> g_vignetteTarget{0.0f};

struct DriverNeeds
{
    float rest, lapar, haus, health, toilet, shower;
};

// ============================================================
// CONFIGURATION
// ============================================================

static constexpr DWORD TELEMETRY_INTERVAL = 5000;
static constexpr DWORD WORKER_INTERVAL = 50;
static constexpr ULONGLONG REST_RECOVERY_DRAIN_DELAY = 20000;
static constexpr float FLY_EFFECT_THRESHOLD = 4.0f;

static constexpr ULONGLONG VIGNETTE_DURATION = 5000;
static constexpr ULONGLONG VIGNETTE_FADE_DURATION = 600;
static constexpr ULONGLONG TOILET_SHAKE_DURATION = 400;
static constexpr ULONGLONG TOILET_SHAKE_GAP = 120;
static constexpr ULONGLONG TOILET_CRITICAL_INTERVAL = 5000;

static constexpr float PERMANENT_VIGNETTE_THRESHOLD = 10.0f;
static constexpr float BLUR_THRESHOLD = 5.0f;

// Threshold pemicu untuk setiap rentang.
// Jumlah threshold = jumlah trigger yang diinginkan.

static constexpr float THRESHOLDS_60_41[] = {
    54.0f, 47.0f};

static constexpr float THRESHOLDS_40_26[] = {
    37.0f, 34.0f, 31.0f, 28.0f};

static constexpr float THRESHOLDS_25_11[] = {
    24.0f, 23.0f, 21.0f, 20.0f, 18.0f,
    17.0f, 15.0f, 14.0f, 12.0f, 11.0f};

// Trigger shake toilet dibagi sesuai jumlah pulse per rentang.
static constexpr float TOILET_THRESHOLDS_50_40[] = {47.0f, 43.0f};
static constexpr float TOILET_THRESHOLDS_40_20[] = {36.0f, 32.0f, 28.0f, 24.0f};
static constexpr float TOILET_THRESHOLDS_20_10[] = {18.0f, 16.0f, 14.0f, 12.0f, 10.0f};
static constexpr float TOILET_THRESHOLDS_10_5[] = {9.0f, 8.0f, 7.0f, 6.0f, 5.0f};

// ============================================================
// STATE MACHINE
// ============================================================

static bool g_baselineReady = false;
static float g_previousNeed = 100.0f;
static float g_currentNeed = 100.0f;

static bool g_triggered60_41[2] = {};
static bool g_triggered40_26[4] = {};
static bool g_triggered25_11[10] = {};

static bool g_pulseActive = false;
static ULONGLONG g_pulseEndTime = 0;

static bool g_permanentVignette = false;
static bool g_blurRequired = false;

static bool g_toiletBaselineReady = false;
static float g_previousToilet = 100.0f;
static bool g_toiletTriggered50_40[2] = {};
static bool g_toiletTriggered40_20[4] = {};
static bool g_toiletTriggered20_10[5] = {};
static bool g_toiletTriggered10_5[5] = {};
static int g_pendingShakePulses = 0;
static bool g_shakePulseActive = false;
static ULONGLONG g_shakePulseEndTime = 0;
static ULONGLONG g_nextShakeTime = 0;
static bool g_toiletCritical = false;
static ULONGLONG g_nextCriticalShakeTime = 0;

// Reset seluruh state ketika game pause / telemetry tidak tersedia.
static void ResetTriggerState()
{
    g_baselineReady = false;
    g_previousNeed = 100.0f;
    g_currentNeed = 100.0f;

    std::fill(
        std::begin(g_triggered60_41),
        std::end(g_triggered60_41),
        false);

    std::fill(
        std::begin(g_triggered40_26),
        std::end(g_triggered40_26),
        false);

    std::fill(
        std::begin(g_triggered25_11),
        std::end(g_triggered25_11),
        false);

    g_pulseActive = false;
    g_pulseEndTime = 0;

    g_permanentVignette = false;
    g_blurRequired = false;

    g_toiletBaselineReady = false;
    g_previousToilet = 100.0f;
    std::fill(std::begin(g_toiletTriggered50_40), std::end(g_toiletTriggered50_40), false);
    std::fill(std::begin(g_toiletTriggered40_20), std::end(g_toiletTriggered40_20), false);
    std::fill(std::begin(g_toiletTriggered20_10), std::end(g_toiletTriggered20_10), false);
    std::fill(std::begin(g_toiletTriggered10_5), std::end(g_toiletTriggered10_5), false);
    g_pendingShakePulses = 0;
    g_shakePulseActive = false;
    g_shakePulseEndTime = 0;
    g_nextShakeTime = 0;
    g_toiletCritical = false;
    g_nextCriticalShakeTime = 0;

    g_vignetteTarget.store(0.0f);
}

template <size_t N>
static void CheckToiletThresholds(
    float previous,
    float current,
    const float (&thresholds)[N],
    bool (&triggered)[N])
{
    for (size_t i = 0; i < N; ++i)
    {
        if (!triggered[i] && previous > thresholds[i] && current <= thresholds[i])
        {
            triggered[i] = true;
            if (g_pendingShakePulses < 64)
                ++g_pendingShakePulses;
        }
    }
}

static void ProcessToiletValue(float current)
{
    if (current > 50.0f)
        std::fill(std::begin(g_toiletTriggered50_40), std::end(g_toiletTriggered50_40), false);
    if (current > 40.0f)
        std::fill(std::begin(g_toiletTriggered40_20), std::end(g_toiletTriggered40_20), false);
    if (current > 20.0f)
        std::fill(std::begin(g_toiletTriggered20_10), std::end(g_toiletTriggered20_10), false);
    if (current > 10.0f)
        std::fill(std::begin(g_toiletTriggered10_5), std::end(g_toiletTriggered10_5), false);

    if (!g_toiletBaselineReady)
    {
        g_previousToilet = current;
        g_toiletBaselineReady = true;
    }
    else
    {
        if (current < g_previousToilet)
        {
            CheckToiletThresholds(g_previousToilet, current, TOILET_THRESHOLDS_50_40, g_toiletTriggered50_40);
            CheckToiletThresholds(g_previousToilet, current, TOILET_THRESHOLDS_40_20, g_toiletTriggered40_20);
            CheckToiletThresholds(g_previousToilet, current, TOILET_THRESHOLDS_20_10, g_toiletTriggered20_10);
            CheckToiletThresholds(g_previousToilet, current, TOILET_THRESHOLDS_10_5, g_toiletTriggered10_5);
        }
        g_previousToilet = current;
    }

    const ULONGLONG now = GetTickCount64();
    if (current < 5.0f)
    {
        if (!g_toiletCritical)
            g_nextCriticalShakeTime = now + TOILET_CRITICAL_INTERVAL;
        g_toiletCritical = true;
    }
    else
    {
        g_toiletCritical = false;
        g_nextCriticalShakeTime = 0;
    }
}

static void UpdateToiletShake()
{
    const ULONGLONG now = GetTickCount64();

    if (g_toiletCritical && now >= g_nextCriticalShakeTime)
    {
        if (g_pendingShakePulses < 64)
            ++g_pendingShakePulses;
        g_nextCriticalShakeTime = now + TOILET_CRITICAL_INTERVAL;
    }

    if (g_shakePulseActive && now >= g_shakePulseEndTime)
    {
        g_shakePulseActive = false;
        g_nextShakeTime = now + TOILET_SHAKE_GAP;
    }

    if (!g_shakePulseActive && g_pendingShakePulses > 0 && now >= g_nextShakeTime)
    {
        --g_pendingShakePulses;
        g_shakePulseActive = true;
        g_shakePulseEndTime = now + TOILET_SHAKE_DURATION;
    }

    float amount = 0.0f;
    float phase = 0.0f;
    if (g_shakePulseActive)
    {
        const ULONGLONG elapsed = TOILET_SHAKE_DURATION - (g_shakePulseEndTime - now);
        const float progress = static_cast<float>(elapsed) / static_cast<float>(TOILET_SHAKE_DURATION);
        const float fade = std::min(1.0f, std::min(progress * 5.0f, (1.0f - progress) * 5.0f));
        amount = std::max(0.0f, fade);
        phase = static_cast<float>(now) / 1000.0f;
    }

    DX11Hook_SetScreenShake(amount, phase);
}

// Memeriksa threshold yang dilewati ketika nilai menurun.
template <size_t N>
static void CheckThresholds(
    float previous,
    float current,
    const float (&thresholds)[N],
    bool (&triggered)[N])
{
    for (size_t i = 0; i < N; i++)
    {
        if (!triggered[i] &&
            previous > thresholds[i] &&
            current <= thresholds[i])
        {
            triggered[i] = true;

            // Langsung mulai satu pulse pada sampel ini.
            // Jika beberapa threshold terlewati sekaligus, pulse tidak diantrekan.
            g_pulseActive = true;
            g_pulseEndTime = GetTickCount64() + VIGNETTE_DURATION;

            if (g_log)
            {
                char buf[128];

                snprintf(
                    buf,
                    sizeof(buf),
                    "DRXexpandTM: VIGNETTE TRIGGER %.1f",
                    thresholds[i]);

                g_log(SCS_LOG_TYPE_message, buf);
            }
        }
    }
}

static void ProcessNeedValue(float current)
{
    // Reset trigger jika kebutuhan pulih melewati
    // batas atas masing-masing rentang.

    if (current > 60.0f)
    {
        std::fill(
            std::begin(g_triggered60_41),
            std::end(g_triggered60_41),
            false);
    }

    if (current > 40.0f)
    {
        std::fill(
            std::begin(g_triggered40_26),
            std::end(g_triggered40_26),
            false);
    }

    if (current > 25.0f)
    {
        std::fill(
            std::begin(g_triggered25_11),
            std::end(g_triggered25_11),
            false);
    }

    // Pembacaan pertama hanya menjadi baseline.
    // Tidak langsung memicu efek saat game dimulai.

    if (!g_baselineReady)
    {
        g_previousNeed = current;
        g_currentNeed = current;
        g_baselineReady = true;
        return;
    }

    // Hanya proses ketika nilai menurun.
    if (current < g_previousNeed)
    {
        CheckThresholds(
            g_previousNeed,
            current,
            THRESHOLDS_60_41,
            g_triggered60_41);

        CheckThresholds(
            g_previousNeed,
            current,
            THRESHOLDS_40_26,
            g_triggered40_26);

        CheckThresholds(
            g_previousNeed,
            current,
            THRESHOLDS_25_11,
            g_triggered25_11);
    }

    g_previousNeed = current;
    g_currentNeed = current;

    // Kondisi permanen.
    g_permanentVignette =
        current <= PERMANENT_VIGNETTE_THRESHOLD;

    // Blur akan dihubungkan ke renderer pada tahap berikutnya.
    g_blurRequired =
        current <= BLUR_THRESHOLD;

    if (g_permanentVignette)
    {
        // Vignette permanen menggantikan pulse sementara.
        g_pulseActive = false;
    }
}

// ============================================================
// VIGNETTE CONTROLLER
// ============================================================

static void UpdateVignette()
{
    const ULONGLONG now = GetTickCount64();
    static float visualIntensity = 0.0f;
    static float visualBlurAmount = 0.0f;
    static ULONGLONG lastUpdateTime = 0;

    const ULONGLONG elapsed = lastUpdateTime == 0
                                  ? WORKER_INTERVAL
                                  : now - lastUpdateTime;
    lastUpdateTime = now;

    float intensity = 0.0f;

    if (g_permanentVignette)
    {
        intensity = 1.0f;
    }
    else
    {
        // Selesaikan pulse yang sedang aktif.
        if (g_pulseActive && now >= g_pulseEndTime)
        {
            g_pulseActive = false;
        }

        if (g_pulseActive)
            intensity = 1.0f;
    }

    // Ramp smoothly toward the desired intensity. Using elapsed wall time
    // keeps the fade duration stable if worker scheduling varies slightly.
    const float maxStep = static_cast<float>(elapsed) /
                          static_cast<float>(VIGNETTE_FADE_DURATION);
    if (visualIntensity < intensity)
        visualIntensity = std::min(intensity, visualIntensity + maxStep);
    else if (visualIntensity > intensity)
        visualIntensity = std::max(intensity, visualIntensity - maxStep);

    const float blurTarget = g_blurRequired ? 1.0f : 0.0f;
    if (visualBlurAmount < blurTarget)
        visualBlurAmount = std::min(blurTarget, visualBlurAmount + maxStep);
    else if (visualBlurAmount > blurTarget)
        visualBlurAmount = std::max(blurTarget, visualBlurAmount - maxStep);

    g_vignetteTarget.store(visualIntensity);

    DX11Hook_SetVignetteIntensity(visualIntensity);
    DX11Hook_SetBlurAmount(visualBlurAmount);
}

// ============================================================
// TELEMETRY
// ============================================================

static bool ReadNeeds(DriverNeeds &out)
{
    HMODULE mod = GetModuleHandleA("TMRealWalk.dll");

    if (!mod)
    {
        if (g_log)
            g_log(
                SCS_LOG_TYPE_message,
                "DRXexpandTM: TMRealWalk NOT FOUND");

        return false;
    }

    const DriverNeeds *p =
        reinterpret_cast<const DriverNeeds *>(
            (BYTE *)mod + 0x7B2700);

    __try
    {
        out = *p;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (g_log)
            g_log(
                SCS_LOG_TYPE_error,
                "DRXexpandTM: MEMORY READ FAILED");

        return false;
    }

    if (g_log)
    {
        char buf[160];

        snprintf(
            buf,
            sizeof(buf),
            "DRXexpandTM RAW: rest=%.2f hunger=%.2f thirst=%.2f health=%.2f toilet=%.2f shower=%.2f",
            out.rest,
            out.lapar,
            out.haus,
            out.health,
            out.toilet,
            out.shower);

        g_log(SCS_LOG_TYPE_message, buf);
    }

    return std::isfinite(out.rest) &&
           std::isfinite(out.lapar) &&
           std::isfinite(out.haus) &&
           std::isfinite(out.health) &&
           std::isfinite(out.toilet) &&
           std::isfinite(out.shower);
}

static bool InRange(float v)
{
    return v >= 0.0f && v <= 100.0f;
}

static bool LooksInitialized(const DriverNeeds &n)
{
    return InRange(n.rest) &&
           InRange(n.lapar) &&
           InRange(n.haus) &&
           InRange(n.health) &&
           InRange(n.toilet) &&
           InRange(n.shower);
}

static void ApplyHealthDrain(float drainPerSecond, float elapsedSeconds)
{
    if (drainPerSecond <= 0.0f || elapsedSeconds <= 0.0f)
        return;

    HMODULE mod = GetModuleHandleA("TMRealWalk.dll");
    if (!mod)
        return;

    volatile float *health = reinterpret_cast<volatile float *>(
        reinterpret_cast<BYTE *>(mod) + 0x7B270C);

    __try
    {
        const float currentHealth = *health;
        if (std::isfinite(currentHealth) &&
            currentHealth > 0.0f &&
            currentHealth <= 100.0f)
        {
            const float nextHealth = std::max(
                0.0f,
                currentHealth - drainPerSecond * elapsedSeconds);
            *health = nextHealth;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (g_log && !g_healthWriteFailedLogged.exchange(true))
            g_log(SCS_LOG_TYPE_error, "DRXexpandTM: HEALTH WRITE FAILED");
    }
}

static int g_pluginModuleAnchor = 0;
static bool g_flyAudioOpened = false;
static bool g_flyAudioErrorLogged = false;
static wchar_t g_flyAudioDirectory[MAX_PATH]{};
static wchar_t g_flyAudioPath[MAX_PATH]{};

static bool EnsureFlyAudioOpen()
{
    if (g_flyAudioOpened)
        return true;

    if (g_flyAudioPath[0] == L'\0')
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&g_pluginModuleAnchor),
                &module))
            return false;

        HRSRC resource = FindResourceW(
            module,
            MAKEINTRESOURCEW(IDR_DRX_FLY_MP3),
            MAKEINTRESOURCEW(10));
        if (!resource)
            return false;

        HGLOBAL loaded = LoadResource(module, resource);
        const DWORD resourceSize = SizeofResource(module, resource);
        const void *resourceData = loaded ? LockResource(loaded) : nullptr;
        if (!resourceData || resourceSize == 0)
            return false;

        const DWORD tempPathLength = GetTempPathW(
            ARRAYSIZE(g_flyAudioDirectory),
            g_flyAudioDirectory);
        if (tempPathLength == 0 || tempPathLength >= ARRAYSIZE(g_flyAudioDirectory))
            return false;

        wchar_t directoryName[64]{};
        if (swprintf_s(
                directoryName,
                L"DRXexpandTM-%lu",
                GetCurrentProcessId()) < 0)
            return false;

        // Rebuild the path from the base returned by GetTempPathW.
        g_flyAudioDirectory[tempPathLength] = L'\0';
        if (wcscat_s(g_flyAudioDirectory, ARRAYSIZE(g_flyAudioDirectory), directoryName) != 0 ||
            wcscat_s(g_flyAudioDirectory, ARRAYSIZE(g_flyAudioDirectory), L"\\") != 0)
            return false;

        if (!CreateDirectoryW(g_flyAudioDirectory, nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;

        if (wcscpy_s(g_flyAudioPath, ARRAYSIZE(g_flyAudioPath), g_flyAudioDirectory) != 0 ||
            wcscat_s(g_flyAudioPath, ARRAYSIZE(g_flyAudioPath), L"fly.mp3") != 0)
            return false;

        HANDLE file = CreateFileW(
            g_flyAudioPath,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD bytesWritten = 0;
        const BOOL writeSucceeded = WriteFile(
            file,
            resourceData,
            resourceSize,
            &bytesWritten,
            nullptr);
        CloseHandle(file);
        if (!writeSucceeded || bytesWritten != resourceSize)
        {
            DeleteFileW(g_flyAudioPath);
            RemoveDirectoryW(g_flyAudioDirectory);
            g_flyAudioPath[0] = L'\0';
            return false;
        }
    }

    wchar_t command[MAX_PATH + 96]{};
    if (swprintf_s(
            command,
            L"open \"%s\" type mpegvideo alias DRXexpandTMFly",
            g_flyAudioPath) < 0)
        return false;

    if (mciSendStringW(command, nullptr, 0, nullptr) != 0)
    {
        if (!g_flyAudioErrorLogged && g_log)
        {
            g_log(SCS_LOG_TYPE_error, "DRXexpandTM: FAILED TO OPEN assets/fly.mp3");
            g_flyAudioErrorLogged = true;
        }
        return false;
    }

    g_flyAudioOpened = true;
    return true;
}

static bool PlayFlyAudio()
{
    if (!EnsureFlyAudioOpen())
        return false;

    mciSendStringW(L"seek DRXexpandTMFly to start", nullptr, 0, nullptr);
    if (mciSendStringW(L"play DRXexpandTMFly repeat", nullptr, 0, nullptr) != 0)
    {
        if (!g_flyAudioErrorLogged && g_log)
        {
            g_log(SCS_LOG_TYPE_error, "DRXexpandTM: FAILED TO PLAY fly audio");
            g_flyAudioErrorLogged = true;
        }
        return false;
    }
    return true;
}

static void StopFlyAudio()
{
    if (g_flyAudioOpened)
        mciSendStringW(L"stop DRXexpandTMFly", nullptr, 0, nullptr);
}

static void CloseFlyAudio()
{
    if (g_flyAudioOpened)
    {
        mciSendStringW(L"stop DRXexpandTMFly", nullptr, 0, nullptr);
        mciSendStringW(L"close DRXexpandTMFly", nullptr, 0, nullptr);
        g_flyAudioOpened = false;
    }
    if (g_flyAudioPath[0] != L'\0')
    {
        DeleteFileW(g_flyAudioPath);
        RemoveDirectoryW(g_flyAudioDirectory);
        g_flyAudioPath[0] = L'\0';
        g_flyAudioDirectory[0] = L'\0';
    }
}

// ============================================================
// WORKER
// ============================================================

static DWORD WINAPI Worker(LPVOID)
{
    if (g_log)
        g_log(
            SCS_LOG_TYPE_message,
            "DRXexpandTM: WORKER STARTED");

    int stableCount = 0;
    bool ready = false;

    ULONGLONG nextTelemetryRead = 0;
    ULONGLONG lastHealthDrainTime = 0;
    ULONGLONG healthDrainResumeTime = 0;
    bool restBaselineReady = false;
    float previousRest = 0.0f;
    float hungerForDrain = 100.0f;
    float thirstForDrain = 100.0f;
    float showerForEffects = 100.0f;
    bool flyAudioPlaying = false;
    bool flyAudioAttempted = false;

    while (g_run.load())
    {
        // Reset state diminta oleh callback pause/start.
        if (g_resetRequested.exchange(false))
        {
            ResetTriggerState();

            stableCount = 0;
            ready = false;
            nextTelemetryRead = 0;
            lastHealthDrainTime = 0;
            healthDrainResumeTime = 0;
            restBaselineReady = false;
            hungerForDrain = 100.0f;
            thirstForDrain = 100.0f;
            showerForEffects = 100.0f;
        }

        // Jangan memproses kebutuhan ketika game pause.
        if (g_paused.load())
        {
            if (ready && g_log)
                g_log(
                    SCS_LOG_TYPE_message,
                    "DRXexpandTM: NOT READY - PAUSED");

            ready = false;
            stableCount = 0;
            lastHealthDrainTime = 0;
            healthDrainResumeTime = 0;
            restBaselineReady = false;

            ResetTriggerState();
            if (flyAudioPlaying)
                StopFlyAudio();
            flyAudioPlaying = false;
            flyAudioAttempted = false;
            showerForEffects = 100.0f;
            DX11Hook_SetShowerValue(100.0f);
            UpdateVignette();
            UpdateToiletShake();

            WaitForSingleObject(
                g_stopEvent,
                WORKER_INTERVAL);

            continue;
        }

        const ULONGLONG now = GetTickCount64();

        // Baca telemetry setiap 5 detik.
        if (now >= nextTelemetryRead)
        {
            nextTelemetryRead =
                now + TELEMETRY_INTERVAL;

            DriverNeeds n;

            bool ok =
                ReadNeeds(n) &&
                LooksInitialized(n);

            if (!ok)
            {
                if (ready && g_log)
                    g_log(
                        SCS_LOG_TYPE_message,
                        "DRXexpandTM: TELEMETRY NOT READY");

                ready = false;
                stableCount = 0;
                lastHealthDrainTime = 0;
                healthDrainResumeTime = 0;
                restBaselineReady = false;

                ResetTriggerState();
                showerForEffects = 100.0f;
            }
            else
            {
                if (stableCount < 2)
                    stableCount++;

                if (!ready && stableCount >= 2)
                {
                    ready = true;

                    ResetTriggerState();

                    if (g_log)
                        g_log(
                            SCS_LOG_TYPE_message,
                            "DRXexpandTM: TMRealWalk READY");
                }

                if (ready)
                {
                    const ULONGLONG restSampleTime = GetTickCount64();
                    if (!restBaselineReady)
                    {
                        previousRest = n.rest;
                        restBaselineReady = true;
                    }
                    else if ((n.lapar <= 0.0f || n.haus <= 0.0f) &&
                             n.rest > previousRest + 4.0f &&
                             restSampleTime >= healthDrainResumeTime)
                    {
                        healthDrainResumeTime =
                            restSampleTime + REST_RECOVERY_DRAIN_DELAY;
                        lastHealthDrainTime = 0;

                        if (g_log)
                            g_log(
                                SCS_LOG_TYPE_message,
                                "DRXexpandTM: REST RECOVERY - HEALTH DRAIN DELAYED 20 SECONDS");
                    }
                    previousRest = n.rest;

                    hungerForDrain = n.lapar;
                    thirstForDrain = n.haus;
                    showerForEffects = n.shower;
                    DX11Hook_SetShowerValue(n.shower);

                    // Satu pengendali untuk hunger dan thirst.
                    // Nilai terendah menjadi prioritas.
                    float effectiveNeed =
                        std::min(n.lapar, n.haus);

                    ProcessNeedValue(effectiveNeed);
                    ProcessToiletValue(n.toilet);

                    if (g_log)
                    {
                        char buf[180];

                        snprintf(
                            buf,
                            sizeof(buf),
                            "DRXexpandTM: hunger=%.1f thirst=%.1f effective=%.1f permanent=%d blur=%d",
                            n.lapar,
                            n.haus,
                            effectiveNeed,
                            g_permanentVignette ? 1 : 0,
                            g_blurRequired ? 1 : 0);

                        g_log(SCS_LOG_TYPE_message, buf);
                    }
                }
            }
        }

        // Update efek setiap 50ms, bukan setiap 5 detik.
        // Ini menjaga durasi pulse tetap akurat.
        UpdateVignette();
        UpdateToiletShake();
        if (!ready)
            DX11Hook_SetShowerValue(100.0f);

        if (ready && showerForEffects < FLY_EFFECT_THRESHOLD)
        {
            if (!flyAudioAttempted)
            {
                flyAudioAttempted = true;
                flyAudioPlaying = PlayFlyAudio();
            }
        }
        else
        {
            if (flyAudioPlaying)
                StopFlyAudio();
            flyAudioPlaying = false;
            flyAudioAttempted = false;
        }

        if (ready)
        {
            const ULONGLONG nowForDrain = GetTickCount64();
            if (nowForDrain < healthDrainResumeTime)
            {
                // Do not accumulate drain time during the post-rest grace period.
                lastHealthDrainTime = nowForDrain;
            }
            else if (lastHealthDrainTime == 0)
            {
                lastHealthDrainTime = nowForDrain;
            }
            else
            {
                const float elapsedSeconds = static_cast<float>(
                    nowForDrain - lastHealthDrainTime) / 1000.0f;
                lastHealthDrainTime = nowForDrain;

                float drainPerSecond = 0.0f;
                if (hungerForDrain <= 0.0f)
                    drainPerSecond += 0.5f;
                if (thirstForDrain <= 0.0f)
                    drainPerSecond += 1.0f;

                ApplyHealthDrain(drainPerSecond, elapsedSeconds);
            }
        }

        if (WaitForSingleObject(
                g_stopEvent,
                WORKER_INTERVAL) == WAIT_OBJECT_0)
        {
            break;
        }
    }

    return 0;
}

// ============================================================
// SCS EVENTS
// ============================================================

static void SCSAPIFUNC OnPaused(
    const scs_event_t event,
    const void *const event_info,
    const scs_context_t context)
{
    g_paused.store(true);
    g_resetRequested.store(true);

    g_vignetteTarget.store(0.0f);

    if (g_log)
        g_log(
            SCS_LOG_TYPE_message,
            "DRXexpandTM: PAUSED");
}

static void SCSAPIFUNC OnStarted(
    const scs_event_t event,
    const void *const event_info,
    const scs_context_t context)
{
    g_paused.store(false);
    g_resetRequested.store(true);

    if (g_log)
        g_log(
            SCS_LOG_TYPE_message,
            "DRXexpandTM: STARTED");
}

// ============================================================
// INITIALIZATION
// ============================================================

SCSAPI_RESULT scs_telemetry_init(
    const scs_u32_t version,
    const scs_telemetry_init_params_t *const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_01)
        return SCS_RESULT_unsupported;

    const auto *p =
        static_cast<const scs_telemetry_init_params_v101_t *>(params);

    g_log = p->common.log;

    if (g_log)
        g_log(
            SCS_LOG_TYPE_message,
            "DRXexpandTM loaded");

    p->register_for_event(
        SCS_TELEMETRY_EVENT_paused,
        OnPaused,
        nullptr);

    p->register_for_event(
        SCS_TELEMETRY_EVENT_started,
        OnStarted,
        nullptr);

    g_run = true;
    g_paused = true;
    g_resetRequested = false;

    g_stopEvent = CreateEventA(
        nullptr,
        TRUE,
        FALSE,
        nullptr);

    if (!g_stopEvent)
    {
        g_run = false;

        if (g_log)
            g_log(
                SCS_LOG_TYPE_error,
                "DRXexpandTM: FAILED TO CREATE STOP EVENT");

        return SCS_RESULT_generic_error;
    }

    g_thread = CreateThread(
        nullptr,
        0,
        Worker,
        nullptr,
        0,
        nullptr);

    if (!g_thread)
    {
        g_run = false;

        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;

        if (g_log)
            g_log(
                SCS_LOG_TYPE_error,
                "DRXexpandTM: FAILED TO CREATE WORKER");

        return SCS_RESULT_generic_error;
    }

    if (g_log)
        g_log(
            SCS_LOG_TYPE_message,
            "DRXexpandTM: WORKER CREATED");

    if (!DX11Hook_Install(g_log))
    {
        if (g_log)
            g_log(
                SCS_LOG_TYPE_error,
                "DRXexpandTM: DX11 HOOK INSTALLATION FAILED");
    }

    return SCS_RESULT_ok;
}

// ============================================================
// SHUTDOWN
// ============================================================

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    g_run = false;

    if (g_stopEvent)
        SetEvent(g_stopEvent);

    if (g_thread)
    {
        WaitForSingleObject(
            g_thread,
            INFINITE);

        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    DX11Hook_Shutdown();
    CloseFlyAudio();

    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    g_vignetteTarget = 0.0f;

    if (g_log)
        g_log(
            SCS_LOG_TYPE_message,
            "DRXexpandTM: SHUTDOWN COMPLETE");
}
