
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <algorithm>
#include "scssdk_telemetry.h"
#include "dx11_hook.h"

static scs_log_t g_log = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_stopEvent = nullptr;

static std::atomic_bool g_run{false};
static std::atomic_bool g_paused{true};
static std::atomic_bool g_resetRequested{false};

static std::atomic<float> g_vignetteTarget{0.0f};

struct DriverNeeds
{
    float lapar, haus, health, toilet;
};

// ============================================================
// CONFIGURATION
// ============================================================

static constexpr DWORD TELEMETRY_INTERVAL = 5000;
static constexpr DWORD WORKER_INTERVAL = 50;

static constexpr ULONGLONG VIGNETTE_DURATION = 5000;
static constexpr ULONGLONG VIGNETTE_FADE_DURATION = 600;

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

    g_vignetteTarget.store(0.0f);
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
            (BYTE *)mod + 0x7B2704);

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
            "DRXexpandTM RAW: %.2f %.2f %.2f %.2f",
            out.lapar,
            out.haus,
            out.health,
            out.toilet);

        g_log(SCS_LOG_TYPE_message, buf);
    }

    return std::isfinite(out.lapar) &&
           std::isfinite(out.haus) &&
           std::isfinite(out.health) &&
           std::isfinite(out.toilet);
}

static bool InRange(float v)
{
    return v >= 0.0f && v <= 100.0f;
}

static bool LooksInitialized(const DriverNeeds &n)
{
    return InRange(n.lapar) &&
           InRange(n.haus) &&
           InRange(n.health) &&
           InRange(n.toilet);
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

    while (g_run.load())
    {
        // Reset state diminta oleh callback pause/start.
        if (g_resetRequested.exchange(false))
        {
            ResetTriggerState();

            stableCount = 0;
            ready = false;
            nextTelemetryRead = 0;
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

            ResetTriggerState();
            UpdateVignette();

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

                ResetTriggerState();
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
                    // Satu pengendali untuk hunger dan thirst.
                    // Nilai terendah menjadi prioritas.
                    float effectiveNeed =
                        std::min(n.lapar, n.haus);

                    ProcessNeedValue(effectiveNeed);

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
