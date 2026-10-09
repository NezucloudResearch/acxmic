// Test tool for the acxmic driver: writes a tone into the driver and records it back through the Windows endpoint.
//   padmic_test status
//   padmic_test selftest <seconds> <freqHz>   writes a tone to the driver, records it from the endpoint, analyses it
//   padmic_test latency <impulses>            one burst per second into silence; time from write to appearing in the recording
//   padmic_test hold <seconds>                opens the control device and holds it (for the exclusive-open / crash tests)
//   padmic_test open                          tries to open the control device: prints the result
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <avrt.h>

#include "padmic_public.h"

static double Qpc() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static void SleepUntil(double t) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    double d = t - Qpc();
    if (d <= 0) return;
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(d * 1e7);
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

static HANDLE OpenDevice() { return CreateFileW(PADMIC_USER_PATH, GENERIC_WRITE | GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr); }

struct Capture {
    std::vector<float> samples;   // channel 0
    std::vector<double> packetQpc; // QPC seconds of each packet's first frame (as reported by the engine)
    std::vector<size_t> packetStart; // sample index of each packet
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    double rate = 0;
    std::string error;
    size_t silentPackets = 0, packets = 0;

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // The endpoint to record: part of its name. PADMIC_ENDPOINT overrides "PadDisplay" (e.g. "CABLE Output" for VB-CABLE).
        wchar_t env[128] = {};
        const std::wstring wanted = GetEnvironmentVariableW(L"PADMIC_ENDPOINT", env, 128) > 0 && *env ? env : L"PadDisplay";
        IMMDeviceEnumerator* en = nullptr;
        IMMDeviceCollection* list = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;
        WAVEFORMATEX* mix = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list);
        UINT n = 0;
        if (SUCCEEDED(hr)) list->GetCount(&n);
        for (UINT i = 0; i < n && !dev; ++i) {
            IMMDevice* d = nullptr;
            list->Item(i, &d);
            IPropertyStore* ps = nullptr;
            d->OpenPropertyStore(STGM_READ, &ps);
            PROPVARIANT v;
            PropVariantInit(&v);
            ps->GetValue(PKEY_Device_FriendlyName, &v);
            std::wstring name = v.vt == VT_LPWSTR ? v.pwszVal : L"";
            wprintf(L"capture endpoint: %s\n", name.c_str());
            PropVariantClear(&v);
            ps->Release();
            if (name.find(wanted) != std::wstring::npos) dev = d; else d->Release();
        }
        if (!dev) { error = "no capture endpoint matching the wanted name"; ready = true; return; }
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
        if (FAILED(hr)) { char b[64]; sprintf_s(b, "capture setup failed 0x%08lx", hr); error = b; ready = true; return; }
        const bool isFloat = mix->wBitsPerSample == 32;
        rate = mix->nSamplesPerSec;
        wprintf(L"capture format: %lu Hz, %u ch, %u bit (%s)\n", mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, isFloat ? L"float" : L"pcm");
        client->Start();
        ready = true;
        while (!stop) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            for (;;) {
                UINT32 next = 0;
                if (FAILED(cap->GetNextPacketSize(&next)) || next == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 pos = 0, qpc = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, &pos, &qpc))) break;
                ++packets; if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++silentPackets;
                packetQpc.push_back(double(qpc) / 1e7);
                packetStart.push_back(samples.size());
                for (UINT32 f = 0; f < frames; ++f) {
                    float s = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        if (isFloat) s = reinterpret_cast<float*>(data)[f * mix->nChannels];
                        else s = reinterpret_cast<int16_t*>(data)[f * mix->nChannels] / 32768.0f;
                    }
                    samples.push_back(s);
                }
                cap->ReleaseBuffer(frames);
            }
        }
        client->Stop();
    }
};

// How late the tone writer woke up, to tell a harness stall (the VM, the scheduler) from a driver problem when the recording has a gap.
static double g_writerLateMaxMs = 0;
static int g_writerLate5 = 0, g_writerLate15 = 0;

// The QPC the audio engine reports uses the same 100 ns clock as QueryPerformanceCounter converted to 100 ns units.
static void Tone(HANDLE dev, double seconds, double freq, std::atomic<bool>& done) {
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task); // like PadDisplay.exe's own writer thread
    const int perPacket = 480;
    double phase = 0;
    const double start = Qpc() + 0.05;
    int packets = int(seconds * 100);
    std::vector<int16_t> buf(perPacket);
    for (int i = 0; i < packets; ++i) {
        SleepUntil(start + i * 0.01);
        const double lateMs = (Qpc() - (start + i * 0.01)) * 1000.0;
        g_writerLateMaxMs = std::max(g_writerLateMaxMs, lateMs);
        if (lateMs > 5) ++g_writerLate5;
        if (lateMs > 15) ++g_writerLate15;
        for (int k = 0; k < perPacket; ++k) {
            buf[k] = int16_t(std::sin(phase) * 0.5 * 32767);
            phase += 2 * 3.14159265358979 * freq / 48000.0;
        }
        DWORD w = 0;
        if (!WriteFile(dev, buf.data(), perPacket * 2, &w, nullptr)) { printf("write failed %lu\n", GetLastError()); break; }
    }
    done = true;
}

// What the driver itself measured: the cost of its timer ticks (DISPATCH_LEVEL) and of the write handler, and how late the
// timer ran each tick. busy/late are per tick; the cpu figure is busy time over the time the ticks cover (10 ms each).
static void PrintTiming(const PADMIC_STATUS& st) {
    if (st.Version < 2 || st.QpcFrequency == 0) { printf("driver timing: not available (interface version %u)\n", st.Version); return; }
    const double us = 1e6 / double(st.QpcFrequency);
    const double ticks = double(st.Ticks ? st.Ticks : 1), writes = double(st.Writes ? st.Writes : 1);
    printf("driver timing: %llu ticks: busy avg %.2f us, max %.1f us; late avg %.1f us, max %.1f us, %llu later than 2 ms; cpu %.4f %% of one core at 100 ticks/s\n",
           st.Ticks, st.TickBusyTotal * us / ticks, st.TickBusyMax * us, st.TickLateTotal * us / ticks, st.TickLateMax * us, st.LateTicks,
           st.TickBusyTotal * us / ticks * 100.0 / 1e6 * 100.0);
    printf("driver timing: %llu writes: busy avg %.2f us, max %.1f us\n", st.Writes, st.WriteBusyTotal * us / writes, st.WriteBusyMax * us);
}

// external = somebody else (PadDisplay.exe) writes the tone: only record and analyse.
static int SelfTest(double seconds, double freq, bool external = false) {
    HANDLE dev = external ? INVALID_HANDLE_VALUE : OpenDevice();
    if (!external && dev == INVALID_HANDLE_VALUE) { printf("cannot open the control device: %lu\n", GetLastError()); return 1; }
    PADMIC_STATUS before{};
    if (!external) { DWORD n = 0; DeviceIoControl(dev, IOCTL_PADMIC_GET_STATUS, nullptr, 0, &before, sizeof(before), &n, nullptr); }
    Capture cap;
    std::thread ct([&] { cap.Run(); });
    while (!cap.ready) Sleep(10);
    if (!cap.error.empty()) { printf("%s\n", cap.error.c_str()); cap.stop = true; ct.join(); return 1; }
    Sleep(500);
    std::atomic<bool> done{false};
    const double t0 = Qpc();
    if (external) Sleep(DWORD(seconds * 1000)); else Tone(dev, seconds, freq, done);
    Sleep(300);
    cap.stop = true;
    ct.join();
    const double t1 = Qpc();

    PADMIC_STATUS st{};
    DWORD got = 0;
    if (!external) {
        DeviceIoControl(dev, IOCTL_PADMIC_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr);
        CloseHandle(dev);
    }

    // Analyse the steady part: from 1 s after the tone began to 0.5 s before it ended.
    const double rate = cap.rate;
    // Find the first sample that is clearly the tone.
    size_t first = 0;
    while (first < cap.samples.size() && std::fabs(cap.samples[first]) < 0.05f) ++first;
    float mx = 0;
    for (float v : cap.samples) mx = std::max(mx, std::fabs(v));
    printf("samples recorded: %zu (%.2f s at %.0f Hz); wall time of the run %.2f s; packets %zu, flagged silent %zu, peak %.4f\n", cap.samples.size(),
           cap.samples.size() / rate, rate, t1 - t0, cap.packets, cap.silentPackets, mx);
    printf("driver: fill %u, running %u, primed %u, underruns %u, drops %u, written %llu, read %llu\n", st.RingFillSamples, st.StreamsRunning, st.Primed,
           st.Underruns, st.Drops, st.SamplesWritten, st.SamplesRead);
    if (first >= cap.samples.size()) { printf("NO TONE in the recording\n"); return 2; }
    size_t last = cap.samples.size();
    while (last > first && std::fabs(cap.samples[last - 1]) < 0.05f) --last; // the tone ends before the recording does
    const size_t a = first + size_t(1.0 * rate);
    const size_t b = last > size_t(0.5 * rate) ? last - size_t(0.5 * rate) : 0;
    printf("tone found from %.2f s to %.2f s of the recording\n", first / rate, last / rate);
    if (b <= a + 1000) { printf("recording too short to analyse\n"); return 2; }
    // Frequency: count rising zero crossings.
    size_t crossings = 0;
    for (size_t i = a + 1; i < b; ++i) if (cap.samples[i - 1] < 0 && cap.samples[i] >= 0) ++crossings;
    const double measured = crossings / ((b - a) / rate);
    // Dropouts: 10 ms blocks whose peak is far below the tone's.
    size_t blocks = 0, quiet = 0, runs = 0;
    bool inRun = false;
    const size_t blk = size_t(rate * 0.01);
    for (size_t i = a; i + blk <= b; i += blk) {
        float peak = 0;
        for (size_t k = 0; k < blk; ++k) peak = std::max(peak, std::fabs(cap.samples[i + k]));
        ++blocks;
        if (peak < 0.2f) { ++quiet; if (!inRun) ++runs; inRun = true; } else inRun = false;
    }
    printf("expected tone %.1f Hz, measured %.2f Hz\n", freq, measured);
    printf("steady part: %zu blocks of 10 ms, %zu quiet (%zu separate dropouts)\n", blocks, quiet, runs);
    printf("driver: fill %u, running %u, primed %u, underruns %u, drops %u, written %llu, read %llu\n", st.RingFillSamples, st.StreamsRunning,
           st.Primed, st.Underruns, st.Drops, st.SamplesWritten, st.SamplesRead);
    const double sentSeconds = seconds;
    const double gotSeconds = double(st.SamplesRead) / 48000.0;
    printf("driver read %.2f s of audio while %.2f s were written (the difference is the idle part before and after)\n", gotSeconds, sentSeconds);
    PrintTiming(st);
    if (!external)
        printf("this run only: underruns +%u, drops +%u; the writer woke up late: max %.1f ms, %d times over 5 ms, %d over 15 ms (the ring's cushion is 20 ms)\n",
               st.Underruns - before.Underruns, st.Drops - before.Drops, g_writerLateMaxMs, g_writerLate5, g_writerLate15);
    return (std::fabs(measured - freq) < 2.0 && runs == 0) ? 0 : 3;
}

static int Latency(int impulses) {
    HANDLE dev = OpenDevice();
    if (dev == INVALID_HANDLE_VALUE) { printf("cannot open the control device: %lu\n", GetLastError()); return 1; }
    Capture cap;
    std::thread ct([&] { cap.Run(); });
    while (!cap.ready) Sleep(10);
    if (!cap.error.empty()) { printf("%s\n", cap.error.c_str()); cap.stop = true; ct.join(); return 1; }
    Sleep(500);
    std::vector<int16_t> silence(480, 0), burst(480);
    for (int k = 0; k < 480; ++k) burst[k] = int16_t(std::sin(k * 2 * 3.14159265358979 * 1000.0 / 48000.0) * 0.9 * 32767);
    const double start = Qpc() + 0.05;
    std::vector<double> writeTimes;
    const int total = (impulses + 1) * 100;
    for (int i = 0; i < total; ++i) {
        SleepUntil(start + i * 0.01);
        const bool isBurst = i >= 100 && i % 100 == 0;
        DWORD w = 0;
        const double before = Qpc();
        WriteFile(dev, isBurst ? burst.data() : silence.data(), 960, &w, nullptr);
        if (isBurst) writeTimes.push_back(before);
    }
    Sleep(300);
    cap.stop = true;
    ct.join();
    CloseHandle(dev);

    // Time of each sample: the engine's QPC of its packet, plus its offset in the packet.
    std::vector<double> lat;
    size_t p = 0;
    for (size_t wi = 0; wi < writeTimes.size(); ++wi) {
        const double target = writeTimes[wi];
        for (size_t pk = p; pk < cap.packetStart.size(); ++pk) {
            const size_t s0 = cap.packetStart[pk];
            const size_t s1 = pk + 1 < cap.packetStart.size() ? cap.packetStart[pk + 1] : cap.samples.size();
            size_t hit = s1;
            for (size_t s = s0; s < s1; ++s) if (std::fabs(cap.samples[s]) > 0.3f && cap.packetQpc[pk] + (s - s0) / cap.rate > target) { hit = s; break; }
            if (hit < s1) {
                const double t = cap.packetQpc[pk] + (hit - s0) / cap.rate;
                lat.push_back((t - target) * 1000.0);
                p = pk;
                break;
            }
        }
    }
    std::sort(lat.begin(), lat.end());
    printf("burst latency (ms, write call to the first loud sample in the endpoint's recording), %zu of %d seen:\n", lat.size(), impulses);
    for (double l : lat) printf("  %.1f\n", l);
    if (!lat.empty()) printf("median %.1f ms\n", lat[lat.size() / 2]);
    return lat.empty() ? 2 : 0;
}

// Starts and stops a recording `n` times (each lasting `holdMs`) while a tone is written all the time: finds leaks and
// state bugs in the stream create/run/pause/destroy path. Prints the driver counters at the end.
static int Cycles(int n, int holdMs) {
    HANDLE dev = OpenDevice();
    if (dev == INVALID_HANDLE_VALUE) { printf("cannot open the control device: %lu\n", GetLastError()); return 1; }
    std::atomic<bool> stopWriter{false};
    std::thread writer([&] {
        std::vector<int16_t> buf(480, 1000);
        const double start = Qpc();
        for (long i = 0; !stopWriter; ++i) {
            SleepUntil(start + i * 0.01);
            DWORD w = 0;
            WriteFile(dev, buf.data(), 960, &w, nullptr);
        }
    });
    int failed = 0;
    for (int i = 0; i < n; ++i) {
        Capture cap;
        std::thread ct([&] { cap.Run(); });
        while (!cap.ready) Sleep(5);
        if (!cap.error.empty()) { ++failed; printf("cycle %d: %s\n", i, cap.error.c_str()); }
        else Sleep(holdMs);
        cap.stop = true;
        ct.join();
    }
    Sleep(300);
    stopWriter = true;
    writer.join();
    PADMIC_STATUS st{};
    DWORD got = 0;
    DeviceIoControl(dev, IOCTL_PADMIC_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr);
    printf("cycles: %d done, %d failed. streams created %u destroyed %u, runs %u pauses %u, destroyedWhileRunning %u, running now %u, underruns %u, drops %u\n",
           n, failed, st.StreamsCreated, st.StreamsDestroyed, st.Runs, st.Pauses, st.DestroyedWhileRunning, st.StreamsRunning, st.Underruns, st.Drops);
    PrintTiming(st);
    CloseHandle(dev);
    return failed ? 1 : 0;
}

// Records for `seconds` and prints when each burst starts (the first loud sample after 0.3 s of quiet) in QueryPerformanceCounter
// seconds, for measuring a writer in another process: mic_client <pin> <s> <hz> <port> burst prints when it sent each burst.
static int BurstRec(double seconds) {
    Capture cap;
    std::thread ct([&] { cap.Run(); });
    while (!cap.ready) Sleep(10);
    if (!cap.error.empty()) { printf("%s\n", cap.error.c_str()); cap.stop = true; ct.join(); return 1; }
    printf("recording %.0f s\n", seconds);
    fflush(stdout);
    Sleep(DWORD(seconds * 1000));
    cap.stop = true;
    ct.join();
    size_t lastLoud = 0, p = 0;
    bool any = false;
    int onsets = 0;
    for (size_t i = 0; i < cap.samples.size(); ++i) {
        if (std::fabs(cap.samples[i]) < 0.05f) continue;
        if (!any || double(i - lastLoud) > 0.3 * cap.rate) {
            while (p + 1 < cap.packetStart.size() && cap.packetStart[p + 1] <= i) ++p;
            printf("ONSET %.6f\n", cap.packetQpc[p] + double(i - cap.packetStart[p]) / cap.rate);
            ++onsets;
        }
        any = true;
        lastLoud = i;
    }
    printf("onsets %d, packets %zu, flagged silent %zu\n", onsets, cap.packets, cap.silentPackets);
    return onsets ? 0 : 2;
}

// The floor of a virtual audio cable: plays a 20 ms tone burst into a playback device (part of its name in PADMIC_RENDER, default
// "CABLE Input") every second, straight from this process through WASAPI shared mode, and prints when each was handed over, in
// QueryPerformanceCounter seconds. Record the cable's other end with `burstrec` (PADMIC_ENDPOINT="CABLE Output"). minPeriod: ask
// for the audio engine's smallest period (IAudioClient3), as PadDisplay does by default.
static int CableWrite(double seconds, bool minPeriod) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t want[128] = {};
    const std::wstring wanted = GetEnvironmentVariableW(L"PADMIC_RENDER", want, 128) > 0 ? want : L"CABLE Input";
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* list = nullptr;
    IMMDevice* dev = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list);
    UINT n = 0;
    list->GetCount(&n);
    for (UINT i = 0; i < n && !dev; ++i) {
        IMMDevice* d = nullptr;
        list->Item(i, &d);
        IPropertyStore* ps = nullptr;
        d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v;
        PropVariantInit(&v);
        ps->GetValue(PKEY_Device_FriendlyName, &v);
        if (v.vt == VT_LPWSTR && std::wstring(v.pwszVal).find(wanted) != std::wstring::npos) dev = d; else d->Release();
        PropVariantClear(&v);
        ps->Release();
    }
    if (!dev) { printf("no playback device matching the wanted name\n"); return 1; }
    IAudioClient* client = nullptr;
    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    WAVEFORMATEX* mix = nullptr;
    client->GetMixFormat(&mix);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;
    if (minPeriod) {
        IAudioClient3* c3 = nullptr;
        if (SUCCEEDED(client->QueryInterface(IID_PPV_ARGS(&c3)))) {
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx)) && SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix, nullptr))) {
                ok = true;
                printf("period %u frames (default %u)\n", mn, def);
            }
        }
    }
    if (!ok && FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr))) {
        // a failed InitializeSharedAudioStream leaves the client unusable: take a new one
        client->Release();
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr))) { printf("cannot open the device\n"); return 1; }
    }
    UINT32 frames = 0;
    client->SetEventHandle(ev);
    client->GetBufferSize(&frames);
    IAudioRenderClient* render = nullptr;
    client->GetService(IID_PPV_ARGS(&render));
    const bool isFloat = mix->wBitsPerSample == 32;
    printf("format %lu Hz, %u ch, %s, buffer %u frames\n", mix->nSamplesPerSec, mix->nChannels, isFloat ? "float" : "16-bit", frames);
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    client->Start();
    const double rate = double(mix->nSamplesPerSec);
    const double stop = Qpc() + seconds;
    uint64_t pos = 0; // frames handed over so far
    const uint64_t perSecond = uint64_t(rate), burstLen = uint64_t(rate * 0.02);
    uint64_t nextBurst = uint64_t(rate); // first burst after 1 s
    while (Qpc() < stop) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        const UINT32 room = frames - padding;
        BYTE* data = nullptr;
        if (room == 0 || FAILED(render->GetBuffer(room, &data))) continue;
        bool burstHere = false;
        for (UINT32 f = 0; f < room; ++f) {
            const uint64_t at = pos + f;
            const bool inBurst = at >= nextBurst && at < nextBurst + burstLen;
            if (at == nextBurst) burstHere = true;
            const float s = inBurst ? float(std::sin(2 * 3.14159265358979 * 1000.0 * double(at - nextBurst) / rate) * 0.5) : 0.f;
            for (UINT32 c = 0; c < mix->nChannels; ++c) {
                if (isFloat) reinterpret_cast<float*>(data)[size_t(f) * mix->nChannels + c] = s;
                else reinterpret_cast<int16_t*>(data)[size_t(f) * mix->nChannels + c] = int16_t(s * 32767);
            }
        }
        if (burstHere) printf("BURST %.6f\n", Qpc());
        render->ReleaseBuffer(room, 0);
        pos += room;
        if (pos >= nextBurst + burstLen) nextBurst += perSecond;
        if (burstHere) fflush(stdout);
    }
    client->Stop();
    return 0;
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "status") {
        HANDLE dev = OpenDevice();
        if (dev == INVALID_HANDLE_VALUE) { printf("cannot open: %lu\n", GetLastError()); return 1; }
        PADMIC_STATUS st{};
        DWORD got = 0;
        BOOL ok = DeviceIoControl(dev, IOCTL_PADMIC_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr);
        printf("ioctl %s, version %u, fill %u, running %u, primed %u, underruns %u, drops %u, written %llu, read %llu\n", ok ? "ok" : "FAILED", st.Version,
               st.RingFillSamples, st.StreamsRunning, st.Primed, st.Underruns, st.Drops, st.SamplesWritten, st.SamplesRead);
        printf("streams created %u destroyed %u, runs %u pauses %u, destroyedWhileRunning %u, ticks %llu\n", st.StreamsCreated, st.StreamsDestroyed,
               st.Runs, st.Pauses, st.DestroyedWhileRunning, st.Ticks);
        PrintTiming(st);
        CloseHandle(dev);
        return ok ? 0 : 1;
    }
    if (mode == "open") {
        HANDLE dev = OpenDevice();
        printf(dev == INVALID_HANDLE_VALUE ? "open failed: %lu\n" : "open ok (%lu)\n", dev == INVALID_HANDLE_VALUE ? GetLastError() : 0);
        if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
        return dev == INVALID_HANDLE_VALUE ? 1 : 0;
    }
    if (mode == "hold") {
        HANDLE dev = OpenDevice();
        if (dev == INVALID_HANDLE_VALUE) { printf("open failed: %lu\n", GetLastError()); return 1; }
        printf("holding\n");
        fflush(stdout);
        Sleep(DWORD((argc > 2 ? atof(argv[2]) : 10) * 1000));
        return 0;
    }
    if (mode == "record") return SelfTest(argc > 2 ? atof(argv[2]) : 10, argc > 3 ? atof(argv[3]) : 1000, true);
    if (mode == "selftest") return SelfTest(argc > 2 ? atof(argv[2]) : 10, argc > 3 ? atof(argv[3]) : 1000);
    if (mode == "latency") return Latency(argc > 2 ? atoi(argv[2]) : 5);
    if (mode == "burstrec") return BurstRec(argc > 2 ? atof(argv[2]) : 10);
    if (mode == "cablewrite") return CableWrite(argc > 2 ? atof(argv[2]) : 10, argc > 3 && std::string(argv[3]) == "min");
    if (mode == "cycles") return Cycles(argc > 2 ? atoi(argv[2]) : 20, argc > 3 ? atoi(argv[3]) : 500);
    printf("usage: padmic_test status | open | hold <s> | selftest <s> <hz> | latency <n> | cycles <n> <ms> | burstrec <s>\n");
    return 64;
}
