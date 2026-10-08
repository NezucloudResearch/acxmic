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
            if (name.find(L"PadDisplay") != std::wstring::npos) dev = d; else d->Release();
        }
        if (!dev) { error = "no PadDisplay capture endpoint"; ready = true; return; }
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

// The QPC the audio engine reports uses the same 100 ns clock as QueryPerformanceCounter converted to 100 ns units.
static void Tone(HANDLE dev, double seconds, double freq, std::atomic<bool>& done) {
    const int perPacket = 480;
    double phase = 0;
    const double start = Qpc() + 0.05;
    int packets = int(seconds * 100);
    std::vector<int16_t> buf(perPacket);
    for (int i = 0; i < packets; ++i) {
        SleepUntil(start + i * 0.01);
        for (int k = 0; k < perPacket; ++k) {
            buf[k] = int16_t(std::sin(phase) * 0.5 * 32767);
            phase += 2 * 3.14159265358979 * freq / 48000.0;
        }
        DWORD w = 0;
        if (!WriteFile(dev, buf.data(), perPacket * 2, &w, nullptr)) { printf("write failed %lu\n", GetLastError()); break; }
    }
    done = true;
}

// external = somebody else (PadDisplay.exe) writes the tone: only record and analyse.
static int SelfTest(double seconds, double freq, bool external = false) {
    HANDLE dev = external ? INVALID_HANDLE_VALUE : OpenDevice();
    if (!external && dev == INVALID_HANDLE_VALUE) { printf("cannot open the control device: %lu\n", GetLastError()); return 1; }
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
    printf("usage: padmic_test status | open | hold <s> | selftest <s> <hz> | latency <n>\n");
    return 64;
}
