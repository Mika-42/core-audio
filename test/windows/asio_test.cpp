//
// Tests unitaires du backend ASIO (mka.audio.backend.asio), Windows, GoogleTest.
//
// Aucun driver ni matériel réel : un FAUX DRIVER IASIO (FakeDriver) est injecté par
// mka::audio::asio_testing::install(). Il enregistre chaque appel (nom + thread), peut
// échouer ou lever une exception sur demande, alloue de vrais buffers ASIO à double
// tampon, et simule le thread audio du driver (tick() manuel ou pompe automatique).
//
// Couverture :
//   Énumération  caps, formats, tailles, cache pendant open, concurrence, jamais d'exception
//   open         tous les codes d'erreur, ordre des appels IASIO, nettoyage après échec
//   Lifecycle    états, start/stop/close en échec, callback avant le retour d'ASIOStart,
//                stop qui attend un callback en cours, destruction dans chaque état
//   Données      6 formats natifs, saturation / NaN, double tampon, canaux, silence
//   Temps réel   zéro allocation, FTZ/DAZ restauré, xruns exacts sans verrou, latence,
//                pas de blocage du callback par le thread de contrôle
//   Messages     asioMessage / sampleRateDidChange / callbacks tardifs
//   Threads      tous les appels IASIO sur UN thread COM STA, stress multi-threads
//
// Variables d'environnement : MKA_SKIP_TIMING=1 désactive les mesures de latence.
//
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define MKA_TEST_X86 1
#endif
#include "iasiodrv.h"

import mka.audio.backend.asio;

namespace mka::audio {
    inline std::ostream &operator<<(std::ostream &os, const ErrorType e) {
        return os << "ErrorType(" << static_cast<int>(e) << ")";
    }
}

// ---------------------------------------------------------------------------
// Compteur d'allocations par thread (opérateur new global remplacé).
// Ne compte que quand un AllocScope est actif sur le thread courant.
// ---------------------------------------------------------------------------
namespace alloc_track {
    inline thread_local bool enabled = false;
    inline thread_local std::size_t count = 0;
}

// Sous sanitizer, new/delete ne sont pas remplacés (ils appartiennent au sanitizer) et les
// tests « zéro allocation » sont ignorés.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"   // faux positif : new/delete remplacés par malloc/free
#endif
void *operator new(std::size_t n) {
    if (alloc_track::enabled) ++alloc_track::count;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

using namespace mka::audio;
using namespace std::chrono_literals;

struct AllocScope {
    AllocScope() { alloc_track::count = 0; alloc_track::enabled = true; }
    ~AllocScope() { alloc_track::enabled = false; }
    std::size_t count() const { return alloc_track::count; }
};

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

::testing::AssertionResult isOk(const Result &r) {
    if (r.has_value()) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "erreur inattendue " << static_cast<int>(r.error());
}
::testing::AssertionResult isError(const Result &r, const ErrorType expected) {
    if (r.has_value()) return ::testing::AssertionFailure() << "succès inattendu, erreur attendue " << static_cast<int>(expected);
    if (r.error() != expected) {
        return ::testing::AssertionFailure() << "erreur " << static_cast<int>(r.error()) << ", attendu " << static_cast<int>(expected);
    }
    return ::testing::AssertionSuccess();
}
#define ASSERT_OK(expr) ASSERT_TRUE(isOk((expr)))
#define ASSERT_ERR(expr, code) ASSERT_TRUE(isError((expr), (code)))

template <class Pred>
bool waitFor(Pred &&pred, const std::chrono::milliseconds timeout = 2000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Exécute f sur un thread à part ; un interblocage fait échouer le test au lieu de le figer.
template <class F>
auto withWatchdog(F &&f, const std::chrono::seconds timeout = 5s) {
    using R = std::invoke_result_t<F &>;
    auto task = std::make_shared<std::packaged_task<R()> >(std::forward<F>(f));
    auto future = task->get_future();
    std::thread([task] { (*task)(); }).detach();
    if (future.wait_for(timeout) != std::future_status::ready) {
        ADD_FAILURE() << "interblocage détecté (aucune réponse en " << timeout.count() << " s)";
        std::_Exit(2);
    }
    return future.get();
}

// ---------------------------------------------------------------------------
// Codec d'échantillons natifs ASIO (pour écrire / relire les buffers du faux driver)
// ---------------------------------------------------------------------------
std::size_t sampleBytes(const ASIOSampleType t) {
    switch (t) {
        case ASIOSTInt16LSB: return 2;
        case ASIOSTInt24LSB: return 3;
        case ASIOSTFloat64LSB: return 8;
        default: return 4;
    }
}

double decodeSample(const ASIOSampleType t, const std::byte *p) {
    switch (t) {
        case ASIOSTInt16LSB: { std::int16_t v; std::memcpy(&v, p, 2); return v / 32768.0; }
        case ASIOSTInt24LSB: {
            std::uint32_t u = std::to_integer<std::uint32_t>(p[0]) | (std::to_integer<std::uint32_t>(p[1]) << 8)
                              | (std::to_integer<std::uint32_t>(p[2]) << 16);
            return (static_cast<std::int32_t>(u << 8) >> 8) / 8388608.0;
        }
        case ASIOSTInt32LSB24: {
            std::uint32_t u; std::memcpy(&u, p, 4);
            return (static_cast<std::int32_t>(u << 8) >> 8) / 8388608.0;
        }
        case ASIOSTInt32LSB: { std::int32_t v; std::memcpy(&v, p, 4); return v / 2147483648.0; }
        case ASIOSTFloat32LSB: { float v; std::memcpy(&v, p, 4); return v; }
        case ASIOSTFloat64LSB: { double v; std::memcpy(&v, p, 8); return v; }
        default: return 0.0;
    }
}

void encodeSample(const ASIOSampleType t, std::byte *p, const double x) {
    switch (t) {
        case ASIOSTInt16LSB: { auto v = static_cast<std::int16_t>(std::lround(x * 32767.0)); std::memcpy(p, &v, 2); break; }
        case ASIOSTInt24LSB: {
            const auto v = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x * 8388607.0)));
            p[0] = static_cast<std::byte>(v & 0xFF); p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
            p[2] = static_cast<std::byte>((v >> 16) & 0xFF);
            break;
        }
        case ASIOSTInt32LSB24: { auto v = static_cast<std::int32_t>(std::lround(x * 8388607.0)); std::memcpy(p, &v, 4); break; }
        case ASIOSTInt32LSB: { auto v = static_cast<std::int32_t>(std::lround(x * 2147483647.0)); std::memcpy(p, &v, 4); break; }
        case ASIOSTFloat32LSB: { auto v = static_cast<float>(x); std::memcpy(p, &v, 4); break; }
        case ASIOSTFloat64LSB: { std::memcpy(p, &x, 8); break; }
        default: break;
    }
}

double rampValue(const std::uint32_t i, const std::uint32_t frames) {
    return -0.9 + 1.8 * static_cast<double>(i) / static_cast<double>(frames - 1);
}

// ---------------------------------------------------------------------------
// Faux driver IASIO
// ---------------------------------------------------------------------------
class FakeDriver final : public IASIO {
public:
    // --- configuration (à régler AVANT open) ---
    long numIn = 2;
    long numOut = 2;
    long minSize = 64, maxSize = 4096, preferred = 256, granularity = -1;
    std::vector<double> rates{44100, 48000, 88200, 96000, 176400, 192000};
    double currentRate = 48000;
    ASIOSampleType defaultType = ASIOSTFloat32LSB;
    std::vector<ASIOSampleType> inTypes, outTypes;   // type par canal (sinon defaultType)
    bool outputReadySupported = true;
    std::chrono::milliseconds startDelay{0};         // ASIOStart lent
    bool tickInsideStart = false;                    // callback depuis un autre thread PENDANT start()
    bool autoPump = false;                           // thread qui appelle bufferSwitch en boucle
    std::chrono::microseconds pumpPeriod{200};
    bool stopDoesNotWait = false;                    // ASIOStop rend la main sans attendre le callback

    // --- observation ---
    struct Call { std::string name; std::thread::id tid; };

    std::atomic<int> liveBuffers{0};                 // createBuffers réussis - disposeBuffers réussis
    std::atomic<std::uint64_t> outputReadyCalls{0};
    std::atomic<std::uint64_t> ticks{0};
    std::atomic<std::size_t> pumpThreadHash{0};
    int aptType = -99;
    void *initHandle = nullptr;
    double lastSetRate = 0;
    long lastCreateNum = 0, lastCreateSize = 0;
    std::vector<ASIOBufferInfo> createdInfos;

    ~FakeDriver() { pumping_ = false; joinPump(); }

    void failOn(const std::string &name, const bool on = true) {
        std::lock_guard l(m_);
        if (on) fail_.insert(name); else fail_.erase(name);
    }
    void throwOn(const std::string &name, const bool on = true) {
        std::lock_guard l(m_);
        if (on) throw_.insert(name); else throw_.erase(name);
    }

    std::vector<Call> calls() const { std::lock_guard l(m_); return calls_; }
    int count(const std::string &name) const {
        std::lock_guard l(m_);
        return static_cast<int>(std::count_if(calls_.begin(), calls_.end(), [&](const Call &c) { return c.name == name; }));
    }
    int indexOf(const std::string &name, int nth = 0) const {
        std::lock_guard l(m_);
        for (std::size_t i = 0; i < calls_.size(); ++i) {
            if (calls_[i].name == name && nth-- == 0) return static_cast<int>(i);
        }
        return -1;
    }
    std::set<std::thread::id> callerThreads() const {
        std::lock_guard l(m_);
        std::set<std::thread::id> s;
        for (const auto &c : calls_) s.insert(c.tid);
        return s;
    }
    void clearCalls() { std::lock_guard l(m_); calls_.clear(); }

    ASIOCallbacks *callbacks() const { return callbacks_; }
    void tick(const long half) {
        if (callbacks_ == nullptr) { ADD_FAILURE() << "tick() avant createBuffers"; return; }
        callbacks_->bufferSwitch(half, ASIOFalse);
        ticks.fetch_add(1);
    }

    // Chaque session (init ... Release) doit tenir sur un seul thread : celui du thread COM de la session.
    bool oneThreadPerSession() const {
        std::lock_guard l(m_);
        std::optional<std::thread::id> session;
        for (const auto &c : calls_) {
            if (c.name == "init") session = c.tid;
            else if (session && c.tid != *session) return false;
            if (c.name == "Release") session.reset();
        }
        return true;
    }

    std::byte *buffer(const bool isInput, const int channel, const int half) const {
        for (const auto &bi : createdInfos) {
            if ((bi.isInput != ASIOFalse) == isInput && bi.channelNum == channel) {
                return static_cast<std::byte *>(bi.buffers[half]);
            }
        }
        return nullptr;
    }
    ASIOSampleType typeOf(const bool isInput, const long ch) const {
        const auto &v = isInput ? inTypes : outTypes;
        return ch < static_cast<long>(v.size()) ? v[static_cast<std::size_t>(ch)] : defaultType;
    }
    std::uint32_t frames() const { return static_cast<std::uint32_t>(lastCreateSize); }

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { record("Release"); return 0; }

    // --- IASIO ---
    ASIOBool init(void *handle) override {
        record("init");
        initHandle = handle;
        APTTYPE apt = APTTYPE_CURRENT; APTTYPEQUALIFIER q;
        if (SUCCEEDED(CoGetApartmentType(&apt, &q))) aptType = static_cast<int>(apt);
        if (shouldThrow("init")) throw std::runtime_error("init");
        return shouldFail("init") ? ASIOFalse : ASIOTrue;
    }
    void getDriverName(char *name) override { std::strcpy(name, "Fake"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char *s) override { std::strcpy(s, "erreur"); }
    ASIOError getChannels(long *i, long *o) override {
        if (auto e = enter("getChannels")) return e;
        *i = numIn; *o = numOut; return ASE_OK;
    }
    ASIOError getLatencies(long *i, long *o) override { *i = *o = 0; return ASE_OK; }
    ASIOError getBufferSize(long *mn, long *mx, long *pf, long *gr) override {
        if (auto e = enter("getBufferSize")) return e;
        *mn = minSize; *mx = maxSize; *pf = preferred; *gr = granularity; return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        if (auto e = enter("canSampleRate")) return e;
        return std::find(rates.begin(), rates.end(), r) != rates.end() ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate *r) override { record("getSampleRate"); *r = currentRate; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (auto e = enter("setSampleRate")) return e;
        lastSetRate = r; currentRate = r; return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource *, long *n) override { *n = 0; return ASE_OK; }
    ASIOError setClockSource(long) override { return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples *, ASIOTimeStamp *) override { return ASE_OK; }
    ASIOError getChannelInfo(ASIOChannelInfo *info) override {
        if (auto e = enter("getChannelInfo")) return e;
        const bool isInput = info->isInput != ASIOFalse;
        if (info->channel < 0 || info->channel >= (isInput ? numIn : numOut)) return ASE_InvalidParameter;
        info->type = typeOf(isInput, info->channel);
        info->isActive = ASIOFalse; info->channelGroup = 0;
        std::strcpy(info->name, "ch");
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo *infos, long n, long size, ASIOCallbacks *cb) override {
        if (auto e = enter("createBuffers")) return e;
        callbacks_ = cb; lastCreateNum = n; lastCreateSize = size;
        store_.clear();
        store_.reserve(static_cast<std::size_t>(n) * 2);
        for (long i = 0; i < n; ++i) {
            const std::size_t bytes = static_cast<std::size_t>(size) * sampleBytes(typeOf(infos[i].isInput != ASIOFalse, infos[i].channelNum));
            for (int h = 0; h < 2; ++h) {
                store_.emplace_back(bytes, std::byte{0});
                infos[i].buffers[h] = store_.back().data();
            }
        }
        createdInfos.assign(infos, infos + n);
        ++liveBuffers;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        if (auto e = enter("disposeBuffers")) return e;
        store_.clear();             // un callback tardif lirait de la mémoire libérée (détectable par ASan)
        createdInfos.clear();
        --liveBuffers;
        return ASE_OK;
    }
    ASIOError controlPanel() override { return ASE_OK; }
    ASIOError future(long, void *) override { return ASE_NotPresent; }
    ASIOError outputReady() override {
        outputReadyCalls.fetch_add(1, std::memory_order_relaxed);   // pas d'allocation : appelé en temps réel
        return outputReadySupported ? ASE_OK : ASE_NotPresent;
    }
    ASIOError start() override {
        if (auto e = enter("start")) return e;
        std::this_thread::sleep_for(startDelay);
        if (tickInsideStart) std::thread([this] { callbacks_->bufferSwitch(0, ASIOFalse); }).join();
        if (autoPump) startPump();
        return ASE_OK;
    }
    ASIOError stop() override {
        if (auto e = enter("stop")) return e;
        pumping_ = false;
        if (!stopDoesNotWait) joinPump();
        return ASE_OK;
    }

private:
    void record(const char *name) {
        std::lock_guard l(m_);
        calls_.push_back({name, std::this_thread::get_id()});
    }
    bool shouldFail(const std::string &n) { std::lock_guard l(m_); return fail_.count(n) != 0; }
    bool shouldThrow(const std::string &n) { std::lock_guard l(m_); return throw_.count(n) != 0; }
    ASIOError enter(const char *name) {
        record(name);
        if (shouldThrow(name)) throw std::runtime_error(name);
        return shouldFail(name) ? ASE_HWMalfunction : ASE_OK;
    }
    void startPump() {
        joinPump();
        pumping_ = true;
        pump_ = std::thread([this] {
            pumpThreadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            long half = 0;
            while (pumping_) {
                tick(half);
                half ^= 1;
                if (pumpPeriod.count() > 0) std::this_thread::sleep_for(pumpPeriod); else std::this_thread::yield();
            }
        });
    }
    void joinPump() { if (pump_.joinable()) pump_.join(); }

    mutable std::mutex m_;
    std::vector<Call> calls_;
    std::set<std::string> fail_, throw_;
    ASIOCallbacks *callbacks_ = nullptr;
    std::vector<std::vector<std::byte> > store_;
    std::atomic<bool> pumping_{false};
    std::thread pump_;
};

// ---------------------------------------------------------------------------
// Callback utilisateur d'observation
// ---------------------------------------------------------------------------
struct Probe {
    using Hook = void (*)(Probe &, const AudioProcessContext &) noexcept;
    Hook hook = nullptr;
    float constant = 0.0f;
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint32_t> frames{0}, inCount{0}, outCount{0};
    std::atomic<bool> outputZeroOnEntry{true};
    std::atomic<std::size_t> threadHash{0};
    std::atomic<unsigned> csrInside{0};
    std::atomic<float> denormResult{1.0f};
    std::atomic<bool> inCallback{false}, release{false};
};

void probeProcess(void *user, const AudioProcessContext &ctx) noexcept {
    auto &p = *static_cast<Probe *>(user);
    p.frames = ctx.frames; p.inCount = ctx.input.count; p.outCount = ctx.output.count;
    p.threadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    for (std::uint32_t c = 0; c < ctx.output.count; ++c) {
        for (std::uint32_t i = 0; i < ctx.frames; ++i) {
            if (ctx.output.channels[c][i] != 0.0f) p.outputZeroOnEntry = false;
        }
    }
    if (p.hook) p.hook(p, ctx);
    p.calls.fetch_add(1, std::memory_order_release);
}

void noopProcess(void *, const AudioProcessContext &) noexcept {}

std::atomic<void *> g_seenUser{nullptr};
void recordUser(void *user, const AudioProcessContext &) noexcept { g_seenUser = user; }

void copyInToOut(Probe &, const AudioProcessContext &c) noexcept {
    const auto n = std::min(c.input.count, c.output.count);
    for (std::uint32_t ch = 0; ch < n; ++ch) std::memcpy(c.output.channels[ch], c.input.channels[ch], c.frames * sizeof(float));
}
void writeConstant(Probe &p, const AudioProcessContext &c) noexcept {
    for (std::uint32_t ch = 0; ch < c.output.count; ++ch)
        for (std::uint32_t i = 0; i < c.frames; ++i) c.output.channels[ch][i] = p.constant;
}
void writeSpecials(Probe &, const AudioProcessContext &c) noexcept {
    const float specials[6] = {2.0f, -2.0f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.0f};
    for (std::uint32_t ch = 0; ch < c.output.count; ++ch)
        for (std::uint32_t i = 0; i < c.frames; ++i) c.output.channels[ch][i] = i < 6 ? specials[i] : 0.0f;
}
void blockUntilReleased(Probe &p, const AudioProcessContext &) noexcept {
    p.inCallback = true;
    while (!p.release) std::this_thread::sleep_for(1ms);
    p.inCallback = false;
}
#if MKA_TEST_X86
void inspectFpu(Probe &p, const AudioProcessContext &) noexcept {
    p.csrInside = _mm_getcsr();
    volatile float tiny = std::numeric_limits<float>::denorm_min();
    volatile float one = 1.0f;           // volatile : empêche le compilateur de supprimer « * 1.0f »
    volatile float r = tiny * one;       // 0 si DAZ est actif
    p.denormResult = r;
}
#endif

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
EndpointConfig makeCfg(const Direction d = Direction::Duplex, const std::uint32_t in = 2, const std::uint32_t out = 2,
                       const SampleRate rate = 48000, const Format fmt = Format::Float32, const BufferSize buf = 256) {
    return EndpointConfig{.id = "FakeASIO", .direction = d, .inputChannels = in, .outputChannels = out,
                          .sampleRate = rate, .format = fmt, .bufferSize = buf};
}

class AsioTest : public ::testing::Test {
protected:
    FakeDriver drv;
    std::atomic<int> loads{0};          // drivers réellement fournis (chacun doit être libéré)
    std::atomic<int> factoryCalls{0};   // appels à la fabrique, y compris pour un nom inconnu
    std::unique_ptr<ASIO> be = std::make_unique<ASIO>();
    Probe probe;

    void SetUp() override { install(); }
    void TearDown() override { be.reset(); asio_testing::reset(); }

    void install(const std::wstring &description = L"Fake ASIO Driver") {
        asio_testing::install({{L"FakeASIO", description}}, [this](const std::wstring &key) -> IASIO * {
            ++factoryCalls;
            if (key != L"FakeASIO") return nullptr;
            ++loads;
            return &drv;
        });
    }

    void openOk(const EndpointConfig &cfg = makeCfg()) { ASSERT_OK(be->open(cfg)); }
    void startOk(Probe::Hook hook = nullptr) {
        probe.hook = hook;
        ASSERT_OK(be->setProcessFunction(probeProcess, &probe));
        ASSERT_OK(be->start());
    }
    void openAndStart(const EndpointConfig &cfg = makeCfg(), Probe::Hook hook = nullptr) {
        openOk(cfg);
        startOk(hook);
    }
    void expectBalanced() {
        EXPECT_EQ(loads.load(), drv.count("Release")) << "référence COM non libérée";
        EXPECT_EQ(drv.liveBuffers.load(), 0) << "buffers ASIO non libérés";
    }
};

// ===========================================================================
// 1. Énumération
// ===========================================================================
TEST_F(AsioTest, EnumerationReportsCapabilities) {
    drv.numIn = 4; drv.numOut = 6;
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].id, "FakeASIO");
    EXPECT_EQ(eps[0].name, "Fake ASIO Driver");
    ASSERT_TRUE(eps[0].input.has_value());
    ASSERT_TRUE(eps[0].output.has_value());
    EXPECT_EQ(eps[0].input->minChannels, 1u);
    EXPECT_EQ(eps[0].input->maxChannels, 4u);
    EXPECT_EQ(eps[0].output->maxChannels, 6u);
    EXPECT_EQ(eps[0].input->sampleRates, std::vector<SampleRate>(supportedSampleRates.begin(), supportedSampleRates.end()));
    EXPECT_EQ(eps[0].input->bufferSizes, std::vector<BufferSize>(supportedBufferSizes.begin(), supportedBufferSizes.end()));
    EXPECT_EQ(eps[0].input->formats, std::vector<Format>{Format::Float32});
    EXPECT_EQ(eps[0].output->formats, std::vector<Format>{Format::Float32});
}

TEST_F(AsioTest, EnumerationNameFallsBackToIdWhenNoDescription) {
    install(L"");
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].name, "FakeASIO");
}

TEST_F(AsioTest, EnumerationOmitsDirectionWithoutChannels) {
    drv.numIn = 0;
    auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_FALSE(eps[0].input.has_value());
    EXPECT_TRUE(eps[0].output.has_value());

    asio_testing::reset(); install();
    drv.numIn = 2; drv.numOut = 0;
    eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_TRUE(eps[0].input.has_value());
    EXPECT_FALSE(eps[0].output.has_value());
}

TEST_F(AsioTest, EnumerationSkipsDriversThatCannotBeUsed) {
    for (const char *what : {"init", "getChannels", "getBufferSize"}) {
        asio_testing::reset(); install();
        drv.failOn(what);
        EXPECT_TRUE(be->getEndPoints().empty()) << what;
        drv.failOn(what, false);
    }
    drv.numIn = 0; drv.numOut = 0;   // ni entrée ni sortie
    asio_testing::reset(); install();
    EXPECT_TRUE(be->getEndPoints().empty());
}

TEST_F(AsioTest, EnumerationWithNoRegisteredDriver) {
    asio_testing::install({}, [](const std::wstring &) -> IASIO * { return nullptr; });
    EXPECT_TRUE(be->getEndPoints().empty());
}

TEST_F(AsioTest, EnumerationSkipsDriverThatFailsToLoad) {
    asio_testing::install({{L"Ghost", L"Introuvable"}}, [](const std::wstring &) -> IASIO * { return nullptr; });
    EXPECT_TRUE(be->getEndPoints().empty());
}

TEST_F(AsioTest, EnumerationNeverThrows) {
    std::vector<Endpoint> eps;
    drv.throwOn("getChannels");
    EXPECT_NO_THROW(eps = be->getEndPoints());
    EXPECT_TRUE(eps.empty());
    expectBalanced();

    drv.throwOn("getChannels", false);
    drv.throwOn("init");
    EXPECT_NO_THROW(eps = be->getEndPoints());
    EXPECT_TRUE(eps.empty());
    expectBalanced();   // init qui lève ne doit pas laisser fuiter la référence COM

    asio_testing::install({{L"Boom", L""}}, [](const std::wstring &) -> IASIO * { throw std::runtime_error("factory"); });
    EXPECT_NO_THROW(eps = be->getEndPoints());
    EXPECT_TRUE(eps.empty());
}

TEST_F(AsioTest, EnumerationLoadsAndReleasesEveryDriverOnADedicatedThread) {
    (void)be->getEndPoints();
    EXPECT_EQ(loads.load(), 1);
    expectBalanced();
    const auto threads = drv.callerThreads();
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_NE(*threads.begin(), std::this_thread::get_id());
}

TEST_F(AsioTest, EnumerationKeepsRegistryOrderAndMultipleDrivers) {
    FakeDriver second;
    second.numIn = 0; second.numOut = 8;
    asio_testing::install({{L"FakeASIO", L"Premier"}, {L"Second", L"Deuxième"}}, [&](const std::wstring &key) -> IASIO * {
        return key == L"FakeASIO" ? static_cast<IASIO *>(&drv) : &second;
    });
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 2u);
    EXPECT_EQ(eps[0].id, "FakeASIO");
    EXPECT_EQ(eps[1].id, "Second");
    EXPECT_FALSE(eps[1].input.has_value());
    EXPECT_EQ(eps[1].output->maxChannels, 8u);
    EXPECT_EQ(second.count("Release"), 1);
}

TEST_F(AsioTest, EnumerationRoundTripsNonAsciiNames) {
    asio_testing::install({{L"Pilote \u00e9\u00e8 \u20ac", L"D\u00e9mo"}}, [&](const std::wstring &) -> IASIO * { return &drv; });
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].id, "Pilote \xC3\xA9\xC3\xA8 \xE2\x82\xAC");
    auto cfg = makeCfg();
    cfg.id = eps[0].id;
    ASSERT_OK(be->open(cfg));      // l'id renvoyé doit pouvoir être rouvert tel quel
    ASSERT_OK(be->close());
}

TEST_F(AsioTest, EnumerationMergesNativeFormatsWithoutDuplicates) {
    drv.numIn = 2; drv.numOut = 3;
    drv.inTypes = {ASIOSTInt16LSB, ASIOSTFloat32LSB};
    drv.outTypes = {ASIOSTInt24LSB, ASIOSTInt32LSB24, ASIOSTInt24LSB};
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].input->formats, (std::vector<Format>{Format::Int16, Format::Float32}));
    EXPECT_EQ(eps[0].output->formats, std::vector<Format>{Format::Int24});
}

TEST_F(AsioTest, EnumerationIgnoresUnsupportedSampleTypes) {
    drv.defaultType = ASIOSTInt32LSB16;
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_TRUE(eps[0].input->formats.empty());
    EXPECT_FALSE(eps[0].input->sampleRates.empty());
}

TEST_F(AsioTest, EnumerationFiltersRatesAndBufferSizesFromTheDriver) {
    drv.rates = {44100, 96000, 12345};
    drv.minSize = 64; drv.maxSize = 1024; drv.granularity = 64;
    const auto eps = be->getEndPoints();
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].input->sampleRates, (std::vector<SampleRate>{44100, 96000}));
    EXPECT_EQ(eps[0].input->bufferSizes, (std::vector<BufferSize>{64, 128, 256, 512, 1024}));

    asio_testing::reset(); install();
    drv.minSize = drv.maxSize = drv.preferred = 480;   // taille fixe hors de la liste supportée
    const auto fixed = be->getEndPoints();
    ASSERT_EQ(fixed.size(), 1u);
    EXPECT_TRUE(fixed[0].input->bufferSizes.empty());
}

TEST_F(AsioTest, EnumerationIsServedFromCacheWhileAnEndpointIsOpen) {
    ASSERT_EQ(be->getEndPoints().size(), 1u);
    openOk();
    const int loadsAfterOpen = loads;
    const auto during = be->getEndPoints();
    EXPECT_EQ(loads.load(), loadsAfterOpen) << "aucun driver ne doit être rechargé pendant qu'un flux est ouvert";
    ASSERT_EQ(during.size(), 1u);
    EXPECT_EQ(during[0].id, "FakeASIO");
    ASSERT_OK(be->close());

    (void)be->getEndPoints();
    EXPECT_EQ(loads.load(), loadsAfterOpen + 1) << "après close, le balayage reprend";
}

TEST_F(AsioTest, EnumerationWithoutPriorScanWhileOpenIsEmptyButSafe) {
    openOk();
    EXPECT_TRUE(be->getEndPoints().empty());
}

TEST_F(AsioTest, EnumerationDoesNotDisturbARunningStream) {
    drv.autoPump = true;
    (void)be->getEndPoints();
    openAndStart();
    ASIO other;
    const auto before = probe.calls.load();
    for (int i = 0; i < 20; ++i) EXPECT_EQ(other.getEndPoints().size(), 1u);
    EXPECT_TRUE(waitFor([&] { return probe.calls.load() > before + 5; })) << "le flux doit continuer à tourner";
    EXPECT_FALSE(be->status().failed);
}

TEST_F(AsioTest, EnumerationIsThreadSafe) {
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20; ++i) if (be->getEndPoints().size() != 1u) ++wrong;
        });
    }
    for (auto &t : threads) t.join();
    EXPECT_EQ(wrong.load(), 0);
    EXPECT_EQ(loads.load(), 160);
    expectBalanced();
}

// ===========================================================================
// 2. open
// ===========================================================================
TEST_F(AsioTest, OpenCallsTheDriverInTheExpectedOrder) {
    drv.currentRate = 44100;   // force setSampleRate
    openOk(makeCfg());
    const int init = drv.indexOf("init"), channels = drv.indexOf("getChannels"), can = drv.indexOf("canSampleRate");
    const int set = drv.indexOf("setSampleRate"), size = drv.indexOf("getBufferSize");
    const int info = drv.indexOf("getChannelInfo"), create = drv.indexOf("createBuffers");
    EXPECT_EQ(init, 0);
    EXPECT_LT(init, channels);
    EXPECT_LT(channels, can);
    EXPECT_LT(can, set);
    EXPECT_LT(set, size);
    EXPECT_LT(size, info);
    EXPECT_LT(info, create);
    EXPECT_EQ(drv.indexOf("Release"), -1) << "le driver reste chargé tant que l'endpoint est ouvert";
    EXPECT_EQ(drv.indexOf("start"), -1) << "open ne démarre pas le flux";
    EXPECT_EQ(drv.lastSetRate, 48000.0);
    EXPECT_NE(drv.initHandle, nullptr);
    ASSERT_OK(be->close());
    EXPECT_LT(drv.indexOf("disposeBuffers"), drv.indexOf("Release"));
}

TEST_F(AsioTest, OpenDoesNotSetSampleRateWhenAlreadyCurrent) {
    drv.currentRate = 48000;
    openOk(makeCfg());
    EXPECT_EQ(drv.count("setSampleRate"), 0);
}

TEST_F(AsioTest, OpenPassesBuffersInputsFirstThenOutputs) {
    drv.numOut = 3;
    openOk(makeCfg(Direction::Duplex, 2, 3, 48000, Format::Float32, 512));
    EXPECT_EQ(drv.lastCreateNum, 5);
    EXPECT_EQ(drv.lastCreateSize, 512);
    ASSERT_EQ(drv.createdInfos.size(), 5u);
    const bool isInput[] = {true, true, false, false, false};
    const long channel[] = {0, 1, 0, 1, 2};
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(drv.createdInfos[i].isInput != ASIOFalse, isInput[i]) << i;
        EXPECT_EQ(drv.createdInfos[i].channelNum, channel[i]) << i;
    }
}

TEST_F(AsioTest, OpenInputOnlyAndOutputOnlyActivateOneDirection) {
    drv.numOut = 4;
    openOk(makeCfg(Direction::Input, 2, 999));
    EXPECT_EQ(drv.lastCreateNum, 2);
    for (const auto &bi : drv.createdInfos) EXPECT_NE(bi.isInput, ASIOFalse);
    ASSERT_OK(be->close());

    openOk(makeCfg(Direction::Output, 999, 4));
    EXPECT_EQ(drv.lastCreateNum, 4);
    for (const auto &bi : drv.createdInfos) EXPECT_EQ(bi.isInput, ASIOFalse);
}

TEST_F(AsioTest, OpenRejectsUnknownOrMalformedIds) {
    auto cfg = makeCfg();
    for (const char *id : {"Inconnu", "", "..\\..\\Autre", "A\\B"}) {
        cfg.id = id;
        ASSERT_ERR(be->open(cfg), ErrorType::EndpointUnavailable);
    }
    EXPECT_EQ(factoryCalls.load(), 1) << "seul « Inconnu » doit atteindre la fabrique (les ids vides ou avec « \\ » sont rejetés avant)";
    EXPECT_EQ(loads.load(), 0);
    expectBalanced();
}

TEST_F(AsioTest, OpenValidatesChannelCounts) {
    ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 0, 2)), ErrorType::ChannelsNotSupported);
    ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 2, 0)), ErrorType::ChannelsNotSupported);
    ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 3, 2)), ErrorType::ChannelsNotSupported);   // max = 2
    ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 2, 3)), ErrorType::ChannelsNotSupported);
    ASSERT_ERR(be->open(makeCfg(Direction::Input, 0, 2)), ErrorType::ChannelsNotSupported);
    ASSERT_ERR(be->open(makeCfg(Direction::Output, 2, 0)), ErrorType::ChannelsNotSupported);
    EXPECT_EQ(drv.count("createBuffers"), 0);
    expectBalanced();
    ASSERT_OK(be->open(makeCfg(Direction::Duplex, 2, 2)));   // exactement le maximum
}

TEST_F(AsioTest, OpenValidatesSampleRates) {
    drv.rates = {44100, 48000, 96000};
    for (const SampleRate rate : supportedSampleRates) {
        const bool ok = rate == 44100 || rate == 48000 || rate == 96000;
        const auto r = be->open(makeCfg(Direction::Duplex, 2, 2, rate));
        if (ok) {
            ASSERT_TRUE(r.has_value()) << rate;
            EXPECT_EQ(drv.currentRate, static_cast<double>(rate));
            ASSERT_OK(be->close());
        } else {
            ASSERT_FALSE(r.has_value()) << rate;
            EXPECT_EQ(r.error(), ErrorType::SampleRateNotSupported) << rate;
        }
    }
    expectBalanced();
}

TEST_F(AsioTest, OpenReportsRejectedSampleRateChange) {
    drv.currentRate = 44100;
    drv.failOn("setSampleRate");
    ASSERT_ERR(be->open(makeCfg()), ErrorType::SampleRateNotSupported);
    expectBalanced();
}

struct SizeCase { long min, max, preferred, granularity; long size; bool valid; };

class AsioBufferSizeTest : public AsioTest, public ::testing::WithParamInterface<SizeCase> {};

TEST_P(AsioBufferSizeTest, OpenAppliesTheDriverBufferSizeRules) {
    const auto c = GetParam();
    drv.minSize = c.min; drv.maxSize = c.max; drv.preferred = c.preferred; drv.granularity = c.granularity;
    const auto r = be->open(makeCfg(Direction::Duplex, 2, 2, 48000, Format::Float32, static_cast<BufferSize>(c.size)));
    if (c.valid) {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(drv.lastCreateSize, c.size);
    } else {
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error(), ErrorType::BufferSizeNotSupported);
        EXPECT_EQ(drv.count("createBuffers"), 0);
    }
}

INSTANTIATE_TEST_SUITE_P(Rules, AsioBufferSizeTest, ::testing::Values(
    // puissances de deux (granularité -1)
    SizeCase{64, 4096, 256, -1, 64, true}, SizeCase{64, 4096, 256, -1, 512, true}, SizeCase{64, 4096, 256, -1, 4096, true},
    SizeCase{64, 4096, 256, -1, 192, false}, SizeCase{64, 4096, 256, -1, 100, false}, SizeCase{64, 4096, 256, -1, 32, false},
    SizeCase{64, 4096, 256, -1, 8192, false}, SizeCase{64, 4096, 256, -1, 0, false},
    SizeCase{128, 1024, 128, -1, 64, false}, SizeCase{128, 1024, 128, -1, 128, true},
    SizeCase{96, 1536, 96, -1, 96, false}, SizeCase{96, 1536, 96, -1, 128, true},
    // pas linéaire (granularité > 0)
    SizeCase{64, 1024, 128, 64, 64, true}, SizeCase{64, 1024, 128, 64, 192, true}, SizeCase{64, 1024, 128, 64, 1024, true},
    SizeCase{64, 1024, 128, 64, 100, false}, SizeCase{64, 1024, 128, 64, 1088, false},
    // taille unique
    SizeCase{480, 480, 480, 0, 480, true}, SizeCase{480, 480, 480, 0, 256, false}, SizeCase{480, 480, 480, 0, 512, false},
    // granularité 0 avec plage : seule la taille préférée est admise
    SizeCase{64, 1024, 256, 0, 256, true}, SizeCase{64, 1024, 256, 0, 64, false}, SizeCase{64, 1024, 256, 0, 512, false}));

// --- formats natifs ---------------------------------------------------------
struct FormatCase { const char *name; ASIOSampleType type; Format format; double tolerance; bool isFloat; };

class AsioFormatTest : public AsioTest, public ::testing::WithParamInterface<FormatCase> {};

const FormatCase kFormats[] = {
    {"Int16", ASIOSTInt16LSB, Format::Int16, 2.0 / 32768, false},
    {"Int24Packed", ASIOSTInt24LSB, Format::Int24, 2.0 / 8388608, false},
    {"Int24In32", ASIOSTInt32LSB24, Format::Int24, 2.0 / 8388608, false},
    {"Int32", ASIOSTInt32LSB, Format::Int32, 3e-7, false},
    {"Float32", ASIOSTFloat32LSB, Format::Float32, 0.0, true},
    {"Float64", ASIOSTFloat64LSB, Format::Float64, 1e-7, true},
};

TEST_P(AsioFormatTest, OpenAcceptsTheNativeFormatAndRejectsTheOthers) {
    const auto c = GetParam();
    drv.defaultType = c.type;
    for (const Format f : supportedFormats) {
        const auto r = be->open(makeCfg(Direction::Duplex, 2, 2, 48000, f));
        if (f == c.format) {
            ASSERT_TRUE(r.has_value());
            ASSERT_OK(be->close());
        } else {
            ASSERT_FALSE(r.has_value());
            EXPECT_EQ(r.error(), ErrorType::FormatNotSupported);
        }
    }
    expectBalanced();
}

TEST_P(AsioFormatTest, ProcessedSamplesSurviveTheConversionRoundTrip) {
    const auto c = GetParam();
    drv.defaultType = c.type;
    openAndStart(makeCfg(Direction::Duplex, 2, 2, 48000, c.format), copyInToOut);
    const auto frames = drv.frames();
    for (int ch = 0; ch < 2; ++ch)
        for (std::uint32_t i = 0; i < frames; ++i)
            encodeSample(c.type, drv.buffer(true, ch, 0) + i * sampleBytes(c.type), rampValue(i, frames) * (ch ? -1 : 1));
    drv.tick(0);
    ASSERT_EQ(probe.calls.load(), 1u);
    for (int ch = 0; ch < 2; ++ch) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            const double in = decodeSample(c.type, drv.buffer(true, ch, 0) + i * sampleBytes(c.type));
            const double out = decodeSample(c.type, drv.buffer(false, ch, 0) + i * sampleBytes(c.type));
            ASSERT_NEAR(out, in, c.tolerance) << "canal " << ch << " échantillon " << i;
        }
    }
}

TEST_P(AsioFormatTest, OutOfRangeSamplesAreSaturatedAndNaNBecomesSilence) {
    const auto c = GetParam();
    drv.defaultType = c.type;
    openAndStart(makeCfg(Direction::Duplex, 2, 2, 48000, c.format), writeSpecials);
    drv.tick(0);
    const auto *out = drv.buffer(false, 1, 0);
    const auto at = [&](int i) { return decodeSample(c.type, out + static_cast<std::size_t>(i) * sampleBytes(c.type)); };
    EXPECT_EQ(at(2), 0.0) << "NaN -> 0";
    EXPECT_EQ(at(5), 0.0);
    if (c.isFloat) {                       // les formats flottants ne saturent pas
        EXPECT_EQ(at(0), 2.0);
        EXPECT_EQ(at(1), -2.0);
        EXPECT_TRUE(std::isinf(at(3)) && at(3) > 0);
        EXPECT_TRUE(std::isinf(at(4)) && at(4) < 0);
    } else {                               // les formats entiers saturent au maximum représentable
        const double tol = c.tolerance;
        EXPECT_GE(at(0), 1.0 - tol); EXPECT_LE(at(0), 1.0);
        EXPECT_LE(at(1), -1.0 + tol); EXPECT_GE(at(1), -1.0);
        EXPECT_GE(at(3), 1.0 - tol);
        EXPECT_LE(at(4), -1.0 + tol);
    }
}

TEST_P(AsioFormatTest, ProcessingDoesNotAllocate) {
    if (kSanitized) GTEST_SKIP() << "compteur d'allocations indisponible sous sanitizer";
    const auto c = GetParam();
    drv.defaultType = c.type;
    drv.numIn = 4; drv.numOut = 4;
    openAndStart(makeCfg(Direction::Duplex, 4, 4, 48000, c.format), copyInToOut);
    drv.tick(0);   // échauffement
    std::size_t allocations = 0;
    {
        AllocScope scope;
        for (int i = 0; i < 2000; ++i) drv.tick(i & 1);
        allocations = scope.count();
    }
    EXPECT_EQ(allocations, 0u) << "le chemin temps réel ne doit jamais allouer";
    EXPECT_GE(probe.calls.load(), 2001u);
}

INSTANTIATE_TEST_SUITE_P(NativeFormats, AsioFormatTest, ::testing::ValuesIn(kFormats),
                         [](const ::testing::TestParamInfo<FormatCase> &i) { return std::string(i.param.name); });

TEST_F(AsioTest, OpenRejectsSampleTypesTheConverterDoesNotHandle) {
    for (const ASIOSampleType type : {ASIOSTInt16MSB, ASIOSTInt24MSB, ASIOSTInt32MSB, ASIOSTFloat32MSB, ASIOSTFloat64MSB,
                                      ASIOSTInt32LSB16, ASIOSTInt32LSB18, ASIOSTInt32LSB20, ASIOSTInt32MSB24,
                                      ASIOSTDSDInt8LSB1}) {
        drv.defaultType = type;
        for (const Format f : supportedFormats) {
            ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 2, 2, 48000, f)), ErrorType::FormatNotSupported);
        }
    }
    expectBalanced();
}

TEST_F(AsioTest, OpenChecksOnlyTheChannelsThatAreActivated) {
    drv.numIn = 8; drv.numOut = 8;
    drv.inTypes = {ASIOSTFloat32LSB, ASIOSTFloat32LSB, ASIOSTInt16LSB, ASIOSTInt16LSB};
    drv.outTypes = {ASIOSTFloat32LSB, ASIOSTFloat32LSB, ASIOSTInt16LSB};
    openOk(makeCfg(Direction::Duplex, 2, 2));              // canaux 0-1 : Float32, les autres sont ignorés
    ASSERT_OK(be->close());
    ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 3, 2)), ErrorType::FormatNotSupported);   // canal 2 = Int16
    ASSERT_ERR(be->open(makeCfg(Direction::Duplex, 2, 3)), ErrorType::FormatNotSupported);
}

struct FailureCase { const char *call; bool onlyIfRateDiffers; ErrorType error; };

class AsioOpenFailureTest : public AsioTest, public ::testing::WithParamInterface<FailureCase> {};

TEST_P(AsioOpenFailureTest, FailedOpenLeavesNothingBehindAndCanBeRetried) {
    const auto c = GetParam();
    if (c.onlyIfRateDiffers) drv.currentRate = 44100;
    for (const bool throws : {false, true}) {
        ErrorType expected = c.error;
        if (throws) {
            drv.throwOn(c.call);
            if (std::string(c.call) == "init") expected = ErrorType::EndpointUnavailable;
            else expected = ErrorType::ConfigurationFailed;
        } else {
            drv.failOn(c.call);
        }
        ASSERT_ERR(be->open(makeCfg()), expected) << c.call << (throws ? " (exception)" : "");
        expectBalanced();
        drv.failOn(c.call, false);
        drv.throwOn(c.call, false);
        ASSERT_OK(be->open(makeCfg())) << "après un échec, open doit pouvoir être rejoué";
        ASSERT_OK(be->close());
        drv.currentRate = 44100;
    }
    expectBalanced();
}

INSTANTIATE_TEST_SUITE_P(DriverCalls, AsioOpenFailureTest, ::testing::Values(
    FailureCase{"init", false, ErrorType::EndpointUnavailable},
    FailureCase{"getChannels", false, ErrorType::ConfigurationFailed},
    FailureCase{"canSampleRate", false, ErrorType::SampleRateNotSupported},
    FailureCase{"setSampleRate", true, ErrorType::SampleRateNotSupported},
    FailureCase{"getBufferSize", false, ErrorType::ConfigurationFailed},
    FailureCase{"getChannelInfo", false, ErrorType::ConfigurationFailed},
    FailureCase{"createBuffers", false, ErrorType::ConfigurationFailed}),
    [](const ::testing::TestParamInfo<FailureCase> &i) { return std::string(i.param.call); });

TEST_F(AsioTest, DisposeBuffersFailureIsReportedByCloseButEverythingIsReleased) {
    openOk();
    drv.failOn("disposeBuffers");
    ASSERT_ERR(be->close(), ErrorType::ConfigurationFailed);
    EXPECT_EQ(drv.count("Release"), 1);
    drv.failOn("disposeBuffers", false);
}

// ===========================================================================
// 3. États et cycle de vie
// ===========================================================================
TEST_F(AsioTest, StateMachineRejectsOperationsInTheWrongState) {
    ASSERT_ERR(be->start(), ErrorType::InvalidState);
    ASSERT_ERR(be->stop(), ErrorType::InvalidState);
    ASSERT_ERR(be->close(), ErrorType::InvalidState);
    openOk();
    ASSERT_ERR(be->open(makeCfg()), ErrorType::InvalidState);
    ASSERT_ERR(be->stop(), ErrorType::InvalidState);
    ASSERT_OK(be->start());
    ASSERT_ERR(be->open(makeCfg()), ErrorType::InvalidState);
    ASSERT_ERR(be->start(), ErrorType::InvalidState);
    ASSERT_ERR(be->close(), ErrorType::InvalidState);
    ASSERT_ERR(be->setProcessFunction(noopProcess), ErrorType::InvalidState);
    ASSERT_OK(be->stop());
    ASSERT_OK(be->setProcessFunction(noopProcess));
    ASSERT_OK(be->close());
    EXPECT_EQ(drv.count("start"), 1);
    EXPECT_EQ(drv.count("stop"), 1);
}

TEST_F(AsioTest, StartBlocksUntilTheDriverHasStarted) {
    drv.startDelay = 120ms;
    openOk();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_OK(be->start());
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 100ms);
}

TEST_F(AsioTest, CallbacksArrivingBeforeAsioStartReturnsAreProcessedWithoutDeadlock) {
    drv.tickInsideStart = true;
    openOk();
    ASSERT_OK(be->setProcessFunction(probeProcess, &probe));
    const auto r = withWatchdog([&] { return be->start(); });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(probe.calls.load(), 1u) << "le callback tourne pendant start(), sans prendre le verrou de contrôle";
}

TEST_F(AsioTest, FailedStartLeavesTheBackendOpenAndRejectsLateCallbacks) {
    openOk();
    ASSERT_OK(be->setProcessFunction(probeProcess, &probe));
    drv.failOn("start");
    ASSERT_ERR(be->start(), ErrorType::ConfigurationFailed);
    drv.tick(0);
    EXPECT_EQ(probe.calls.load(), 0u) << "un callback après un start raté ne doit pas atteindre l'utilisateur";
    drv.failOn("start", false);
    ASSERT_OK(be->start());
    drv.tick(0);
    EXPECT_EQ(probe.calls.load(), 1u);
}

TEST_F(AsioTest, StopMakesLateCallbacksHarmless) {
    openAndStart();
    drv.tick(0);
    EXPECT_EQ(probe.calls.load(), 1u);
    ASSERT_OK(be->stop());
    drv.tick(0); drv.tick(1);
    EXPECT_EQ(probe.calls.load(), 1u);
    ASSERT_OK(be->close());
    drv.callbacks()->bufferSwitch(0, ASIOFalse);   // après dispose : ni crash ni accès aux buffers libérés
    EXPECT_EQ(probe.calls.load(), 1u);
}

TEST_F(AsioTest, FailedStopKeepsTheStreamRunning) {
    openAndStart();
    drv.failOn("stop");
    ASSERT_ERR(be->stop(), ErrorType::ConfigurationFailed);
    drv.tick(0);
    EXPECT_EQ(probe.calls.load(), 1u) << "le flux est toujours actif après un stop raté";
    drv.failOn("stop", false);
    ASSERT_OK(be->stop());
    drv.tick(0);
    EXPECT_EQ(probe.calls.load(), 1u);
}

TEST_F(AsioTest, StopWaitsForACallbackThatIsStillRunning) {
    drv.autoPump = true;
    drv.stopDoesNotWait = true;      // le driver rend la main alors qu'un callback est en cours
    openAndStart(makeCfg(), blockUntilReleased);
    ASSERT_TRUE(waitFor([&] { return probe.inCallback.load(); }));

    auto stopping = std::async(std::launch::async, [&] { return be->stop(); });
    EXPECT_EQ(stopping.wait_for(150ms), std::future_status::timeout) << "stop() doit attendre la fin du callback";
    EXPECT_EQ(drv.count("disposeBuffers"), 0);

    probe.release = true;
    ASSERT_EQ(stopping.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(stopping.get().has_value());
    const auto calls = probe.calls.load();
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(probe.calls.load(), calls) << "plus aucun callback après le retour de stop()";
    ASSERT_OK(be->close());
}

TEST_F(AsioTest, RestartingDoesNotRecreateBuffersAndKeepsProcessing) {
    openOk();
    ASSERT_OK(be->setProcessFunction(probeProcess, &probe));
    for (int i = 0; i < 100; ++i) {
        ASSERT_OK(be->start());
        drv.tick(i & 1);
        ASSERT_OK(be->stop());
    }
    EXPECT_EQ(probe.calls.load(), 100u);
    EXPECT_EQ(drv.count("createBuffers"), 1);
    EXPECT_EQ(drv.count("start"), 100);
    ASSERT_OK(be->close());
    expectBalanced();
}

TEST_F(AsioTest, ManyOpenCloseCyclesDoNotLeak) {
    for (int i = 0; i < 50; ++i) {
        openOk(makeCfg(Direction::Duplex, 2, 2, i % 2 ? 44100 : 48000));
        ASSERT_OK(be->start());
        ASSERT_OK(be->stop());
        ASSERT_OK(be->close());
    }
    EXPECT_EQ(loads.load(), 50);
    expectBalanced();
}

class AsioDestroyTest : public AsioTest, public ::testing::WithParamInterface<int> {};

TEST_P(AsioDestroyTest, DestroyingTheBackendInAnyStateReleasesEverything) {
    const int state = GetParam();   // 0 fermé, 1 ouvert, 2 en marche
    drv.autoPump = true;
    {
        ASIO local;
        if (state >= 1) { ASSERT_OK(local.open(makeCfg())); }
        if (state >= 2) {
            ASSERT_OK(local.setProcessFunction(probeProcess, &probe));
            ASSERT_OK(local.start());
            ASSERT_TRUE(waitFor([&] { return probe.calls.load() > 3; }));
        }
    }
    expectBalanced();
    if (state == 2) { EXPECT_GE(drv.count("stop"), 1); }
    ASSERT_OK(be->open(makeCfg())) << "la place ASIO doit être libérée par le destructeur";
}

INSTANTIATE_TEST_SUITE_P(States, AsioDestroyTest, ::testing::Values(0, 1, 2));

// ===========================================================================
// 4. Données
// ===========================================================================
TEST_F(AsioTest, ProcessContextMatchesTheConfiguration) {
    openAndStart(makeCfg(Direction::Duplex, 2, 2, 48000, Format::Float32, 512));
    drv.tick(0);
    EXPECT_EQ(probe.frames.load(), 512u);
    EXPECT_EQ(probe.inCount.load(), 2u);
    EXPECT_EQ(probe.outCount.load(), 2u);

    ASSERT_OK(be->stop()); ASSERT_OK(be->close());
    openAndStart(makeCfg(Direction::Input, 2, 0));
    drv.tick(0);
    EXPECT_EQ(probe.inCount.load(), 2u);
    EXPECT_EQ(probe.outCount.load(), 0u);

    ASSERT_OK(be->stop()); ASSERT_OK(be->close());
    openAndStart(makeCfg(Direction::Output, 0, 2));
    drv.tick(0);
    EXPECT_EQ(probe.inCount.load(), 0u);
    EXPECT_EQ(probe.outCount.load(), 2u);
}

TEST_F(AsioTest, UserPointerReachesTheCallbackUnchanged) {
    openOk();
    int marker = 0;
    ASSERT_OK(be->setProcessFunction(recordUser, &marker));
    ASSERT_OK(be->start());
    drv.tick(0);
    EXPECT_EQ(g_seenUser.load(), &marker);
}

TEST_F(AsioTest, DoubleBufferIndexSelectsTheCorrectHalf) {
    openAndStart(makeCfg(), copyInToOut);
    const auto n = drv.frames();
    for (std::uint32_t i = 0; i < n; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            encodeSample(ASIOSTFloat32LSB, drv.buffer(true, ch, 0) + i * 4, 0.25);
            encodeSample(ASIOSTFloat32LSB, drv.buffer(true, ch, 1) + i * 4, -0.25);
            encodeSample(ASIOSTFloat32LSB, drv.buffer(false, ch, 0) + i * 4, 9.0);
            encodeSample(ASIOSTFloat32LSB, drv.buffer(false, ch, 1) + i * 4, 9.0);
        }
    }
    drv.tick(0);
    for (std::uint32_t i = 0; i < n; ++i) {
        ASSERT_EQ(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, 0, 0) + i * 4), 0.25);
        ASSERT_EQ(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, 0, 1) + i * 4), 9.0) << "l'autre moitié ne doit pas être touchée";
    }
    drv.tick(1);
    for (std::uint32_t i = 0; i < n; ++i) ASSERT_EQ(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, 1, 1) + i * 4), -0.25);
}

TEST_F(AsioTest, ChannelsStayIsolatedFromEachOther) {
    drv.numIn = 8; drv.numOut = 8;
    openAndStart(makeCfg(Direction::Duplex, 8, 8), copyInToOut);
    const auto n = drv.frames();
    for (int ch = 0; ch < 8; ++ch)
        for (std::uint32_t i = 0; i < n; ++i) encodeSample(ASIOSTFloat32LSB, drv.buffer(true, ch, 0) + i * 4, (ch + 1) * 0.1);
    drv.tick(0);
    for (int ch = 0; ch < 8; ++ch)
        for (std::uint32_t i = 0; i < n; ++i)
            ASSERT_FLOAT_EQ(static_cast<float>(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, ch, 0) + i * 4)), static_cast<float>((ch + 1) * 0.1));
}

TEST_F(AsioTest, Int32In24ReadingIgnoresGarbageInTheTopByte) {
    drv.defaultType = ASIOSTInt32LSB24;
    openAndStart(makeCfg(Direction::Duplex, 2, 2, 48000, Format::Int24), copyInToOut);
    for (std::uint32_t i = 0; i < drv.frames(); ++i) {
        const std::uint32_t raw = 0xAB400000u;   // 0x400000 = +0.5, octet de poids fort parasite
        std::memcpy(drv.buffer(true, 0, 0) + i * 4, &raw, 4);
    }
    drv.tick(0);
    EXPECT_NEAR(decodeSample(ASIOSTInt32LSB24, drv.buffer(false, 0, 0)), 0.5, 2.0 / 8388608);
}

TEST_F(AsioTest, OutputIsZeroedBeforeEveryCallback) {
    openAndStart(makeCfg(), writeConstant);
    probe.constant = 0.7f;
    for (int i = 0; i < 5; ++i) drv.tick(i & 1);
    EXPECT_TRUE(probe.outputZeroOnEntry.load()) << "le callback doit toujours recevoir une sortie à zéro";
    EXPECT_FLOAT_EQ(static_cast<float>(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, 0, 0))), 0.7f);
}

TEST_F(AsioTest, WithoutCallbackTheDriverReceivesSilenceNotStaleData) {
    openOk();
    ASSERT_OK(be->start());
    for (int ch = 0; ch < 2; ++ch)
        for (std::uint32_t i = 0; i < drv.frames(); ++i) encodeSample(ASIOSTFloat32LSB, drv.buffer(false, ch, 0) + i * 4, 9.0);
    drv.tick(0);
    for (std::uint32_t i = 0; i < drv.frames(); ++i) ASSERT_EQ(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, 1, 0) + i * 4), 0.0);
}

TEST_F(AsioTest, TimeInfoCallbackBehavesLikeTheSimpleOne) {
    openAndStart();
    ASIOTime t{};
    EXPECT_EQ(drv.callbacks()->bufferSwitchTimeInfo(&t, 0, ASIOFalse), nullptr);
    EXPECT_EQ(probe.calls.load(), 1u);
}

TEST_F(AsioTest, InvalidBufferIndicesAreIgnored) {
    openAndStart();
    drv.callbacks()->bufferSwitch(2, ASIOFalse);
    drv.callbacks()->bufferSwitch(-1, ASIOFalse);
    drv.callbacks()->bufferSwitch(1000000, ASIOFalse);
    EXPECT_EQ(probe.calls.load(), 0u);
}

TEST_F(AsioTest, OutputReadyIsDetectedOnceAndSignalledEveryCycle) {
    openAndStart();
    EXPECT_EQ(drv.outputReadyCalls.load(), 1u) << "une fois à l'ouverture pour détecter le support";
    for (int i = 0; i < 5; ++i) drv.tick(i & 1);
    EXPECT_EQ(drv.outputReadyCalls.load(), 6u);
}

TEST_F(AsioTest, OutputReadyIsNeverCalledWhenUnsupportedOrWithoutOutputs) {
    drv.outputReadySupported = false;
    openAndStart();
    for (int i = 0; i < 5; ++i) drv.tick(i & 1);
    EXPECT_EQ(drv.outputReadyCalls.load(), 1u) << "seule la détection initiale";
    ASSERT_OK(be->stop()); ASSERT_OK(be->close());

    drv.outputReadyCalls = 0; drv.outputReadySupported = true;
    openAndStart(makeCfg(Direction::Input, 2, 0));
    for (int i = 0; i < 5; ++i) drv.tick(i & 1);
    EXPECT_EQ(drv.outputReadyCalls.load(), 0u);
}

TEST_F(AsioTest, LargeConfigurationWorks) {
    drv.numIn = 32; drv.numOut = 32;
    openAndStart(makeCfg(Direction::Duplex, 32, 32, 48000, Format::Float32, 4096), copyInToOut);
    for (std::uint32_t i = 0; i < 4096; ++i) encodeSample(ASIOSTFloat32LSB, drv.buffer(true, 31, 0) + i * 4, rampValue(i, 4096));
    drv.tick(0);
    EXPECT_EQ(probe.inCount.load(), 32u);
    EXPECT_FLOAT_EQ(static_cast<float>(decodeSample(ASIOSTFloat32LSB, drv.buffer(false, 31, 0) + 4095 * 4)), static_cast<float>(rampValue(4095, 4096)));
}

// ===========================================================================
// 5. Temps réel
// ===========================================================================
TEST_F(AsioTest, RealtimeCallbackRunsOnTheDriverThreadNotOnControlOrComThreads) {
    drv.autoPump = true;
    openAndStart();
    ASSERT_TRUE(waitFor([&] { return probe.calls.load() > 3; }));
    EXPECT_EQ(probe.threadHash.load(), drv.pumpThreadHash.load());
    EXPECT_NE(probe.threadHash.load(), std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto threads = drv.callerThreads();     // thread COM : seul appelant des méthodes de contrôle
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_NE(std::hash<std::thread::id>{}(*threads.begin()), probe.threadHash.load());
}

TEST_F(AsioTest, RealtimeStatusReportsUnknownBecauseTheThreadBelongsToTheDriver) {
    openAndStart();
    drv.tick(0);
    EXPECT_EQ(be->status().realtime, RealtimeState::Unknown);
}

#if MKA_TEST_X86
TEST_F(AsioTest, RealtimeDenormalsAreFlushedInsideTheCallbackAndRestoredAfter) {
    openAndStart(makeCfg(), inspectFpu);
    const unsigned original = _mm_getcsr();
    _mm_setcsr((original | 0x6000u) & ~0x8040u);     // arrondi vers zéro, FTZ/DAZ coupés : état « étranger »
    const unsigned before = _mm_getcsr();
    drv.tick(0);
    const unsigned after = _mm_getcsr();
    _mm_setcsr(original);
    EXPECT_EQ(probe.csrInside.load() & 0x8040u, 0x8040u) << "FTZ et DAZ actifs dans le callback";
    EXPECT_EQ(probe.denormResult.load(), 0.0f);
    EXPECT_EQ(after, before) << "le registre du thread du driver doit être restauré à l'identique";
}
#endif

TEST_F(AsioTest, RealtimeXrunCountingIsExactUnderConcurrency) {
    openAndStart();
    auto *cb = drv.callbacks();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([=] {
            for (int i = 0; i < 5000; ++i) cb->asioMessage(i & 1 ? kAsioOverload : kAsioResyncRequest, 0, nullptr, nullptr);
        });
    }
    for (auto &t : threads) t.join();
    EXPECT_EQ(be->status().xruns, 40000u);
    EXPECT_FALSE(be->status().failed);
}

TEST_F(AsioTest, RealtimeStatusIsLockFreeWhileStartIsBlockedInTheDriver) {
    drv.startDelay = 400ms;
    openOk();
    std::thread starter([&] { ASSERT_OK(be->start()); });
    std::this_thread::sleep_for(60ms);              // start() tient le mutex de contrôle et attend le driver
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) (void)be->status();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 50ms) << "status() ne doit prendre aucun verrou";
    starter.join();
}

TEST_F(AsioTest, RealtimeNotificationsDoNotAllocate) {
    if (kSanitized) GTEST_SKIP() << "compteur d'allocations indisponible sous sanitizer";
    openAndStart();
    auto *cb = drv.callbacks();
    std::size_t allocations = 0;
    {
        AllocScope scope;
        for (int i = 0; i < 1000; ++i) {
            cb->asioMessage(kAsioOverload, 0, nullptr, nullptr);
            cb->asioMessage(kAsioSelectorSupported, kAsioOverload, nullptr, nullptr);
            cb->sampleRateDidChange(48000.0);
            drv.tick(i & 1);
        }
        allocations = scope.count();
    }
    EXPECT_EQ(allocations, 0u);
}

TEST_F(AsioTest, RealtimeBackendOverheadStaysSmallComparedToThePeriod) {
    if (kSanitized || std::getenv("MKA_SKIP_TIMING")) GTEST_SKIP() << "mesure désactivée";
    drv.defaultType = ASIOSTInt24LSB;
    drv.numIn = 8; drv.numOut = 8;
    openOk(makeCfg(Direction::Duplex, 8, 8, 48000, Format::Int24, 256));
    ASSERT_OK(be->setProcessFunction(noopProcess));
    ASSERT_OK(be->start());
    for (int i = 0; i < 500; ++i) drv.tick(i & 1);
    std::vector<double> us(20000);
    for (std::size_t i = 0; i < us.size(); ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        drv.tick(static_cast<long>(i & 1));
        us[i] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    }
    std::sort(us.begin(), us.end());
    const double periodUs = 256.0 / 48000.0 * 1e6;
    RecordProperty("p50_us", static_cast<int>(us[us.size() / 2]));
    RecordProperty("p99_us", static_cast<int>(us[us.size() * 99 / 100]));
    EXPECT_LT(us[us.size() / 2], 0.05 * periodUs) << "médiane";
    EXPECT_LT(us[us.size() * 99 / 100], 0.50 * periodUs) << "p99";
}

TEST_F(AsioTest, RealtimeSustainedRunWithConcurrentObserversStaysHealthy) {
    drv.autoPump = true;
    drv.pumpPeriod = 100us;
    (void)be->getEndPoints();
    openAndStart();
    std::atomic<bool> stopObservers{false};
    std::thread observer([&] { while (!stopObservers) { (void)be->status(); (void)be->getEndPoints(); } });
    std::this_thread::sleep_for(300ms);
    stopObservers = true;
    observer.join();
    EXPECT_GT(probe.calls.load(), 200u);
    EXPECT_EQ(be->status().xruns, 0u);
    EXPECT_FALSE(be->status().failed);
}

// ===========================================================================
// 6. Messages et callbacks du driver
// ===========================================================================
TEST_F(AsioTest, MessagesDeclareTheSupportedSelectors) {
    openOk();
    auto *cb = drv.callbacks();
    const long supported[] = {kAsioEngineVersion, kAsioResetRequest, kAsioBufferSizeChange, kAsioResyncRequest,
                              kAsioLatenciesChanged, kAsioOverload};
    for (const long sel : supported) {
        EXPECT_EQ(cb->asioMessage(kAsioSelectorSupported, sel, nullptr, nullptr), 1) << sel;
    }
    const long unsupported[] = {kAsioSupportsTimeInfo, kAsioSupportsTimeCode, kAsioMMCCommand, kAsioSupportsInputMonitor,
                                kAsioSupportsInputGain, kAsioSupportsInputMeter, kAsioSupportsOutputGain,
                                kAsioSupportsOutputMeter, 9999};
    for (const long sel : unsupported) {
        EXPECT_EQ(cb->asioMessage(kAsioSelectorSupported, sel, nullptr, nullptr), 0) << sel;
    }
    EXPECT_EQ(cb->asioMessage(kAsioEngineVersion, 0, nullptr, nullptr), 2);
    EXPECT_EQ(cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr), 0);
    EXPECT_EQ(cb->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr), 1);
    EXPECT_EQ(cb->asioMessage(9999, 0, nullptr, nullptr), 0);
    EXPECT_FALSE(be->status().failed);
    EXPECT_EQ(be->status().xruns, 0u);
}

TEST_F(AsioTest, MessagesOverloadAndResyncCountAsXruns) {
    openAndStart();
    EXPECT_EQ(drv.callbacks()->asioMessage(kAsioOverload, 0, nullptr, nullptr), 1);
    EXPECT_EQ(drv.callbacks()->asioMessage(kAsioResyncRequest, 0, nullptr, nullptr), 1);
    EXPECT_EQ(be->status().xruns, 2u);
    EXPECT_FALSE(be->status().failed);
}

TEST_F(AsioTest, MessagesResetAndBufferSizeChangeMarkTheStreamAsFailed) {
    const long failing[] = {kAsioResetRequest, kAsioBufferSizeChange};
    for (const long selector : failing) {
        openAndStart();
        EXPECT_FALSE(be->status().failed);
        EXPECT_EQ(drv.callbacks()->asioMessage(selector, 0, nullptr, nullptr), 1);
        EXPECT_TRUE(be->status().failed);
        EXPECT_EQ(be->status().xruns, 0u);
        ASSERT_OK(be->stop());
        ASSERT_OK(be->close());
        ASSERT_OK(be->setProcessFunction(nullptr));
        openOk(); ASSERT_OK(be->start());
        EXPECT_FALSE(be->status().failed) << "start() remet l'état d'échec à zéro";
        ASSERT_OK(be->stop()); ASSERT_OK(be->close());
    }
}

TEST_F(AsioTest, XrunCounterResetsOnRestart) {
    openAndStart();
    drv.callbacks()->asioMessage(kAsioOverload, 0, nullptr, nullptr);
    EXPECT_EQ(be->status().xruns, 1u);
    ASSERT_OK(be->stop());
    ASSERT_OK(be->start());
    EXPECT_EQ(be->status().xruns, 0u);
}

TEST_F(AsioTest, SampleRateChangeToAnotherRateFailsTheStream) {
    openAndStart();
    drv.callbacks()->sampleRateDidChange(48000.0);
    drv.callbacks()->sampleRateDidChange(48000.4);   // arrondi : même cadence
    EXPECT_FALSE(be->status().failed);
    drv.callbacks()->sampleRateDidChange(44100.0);
    EXPECT_TRUE(be->status().failed);
}

TEST_F(AsioTest, LostClockFailsTheStream) {
    openAndStart();
    drv.callbacks()->sampleRateDidChange(0.0);
    EXPECT_TRUE(be->status().failed);
}

TEST_F(AsioTest, LateCallbacksAfterCloseAreHarmlessAndDoNotAffectTheNextSession) {
    openAndStart();
    auto *cb = drv.callbacks();
    ASSERT_OK(be->stop());
    ASSERT_OK(be->close());
    EXPECT_NO_THROW({
        cb->bufferSwitch(0, ASIOFalse);
        cb->bufferSwitchTimeInfo(nullptr, 1, ASIOFalse);
        cb->sampleRateDidChange(96000.0);
        EXPECT_EQ(cb->asioMessage(kAsioResetRequest, 0, nullptr, nullptr), 1);
        EXPECT_EQ(cb->asioMessage(kAsioOverload, 0, nullptr, nullptr), 1);
    });
    EXPECT_FALSE(be->status().failed);
    EXPECT_EQ(be->status().xruns, 0u);
    openOk();
    EXPECT_FALSE(be->status().failed);
}

// ===========================================================================
// 7. Événements (thread dispatcher, hors temps réel)
// ===========================================================================
struct EventLog {
    std::mutex m;
    std::vector<Event> events;
    std::vector<std::thread::id> threads;
    std::optional<ErrorType> stopFromHandler;
    Backend *backend = nullptr;
};

void onEvent(void *user, const Event &e) noexcept {
    auto &log = *static_cast<EventLog *>(user);
    const auto r = log.backend->stop();         // interdit depuis le handler : doit répondre InvalidState
    std::lock_guard l(log.m);
    log.events.push_back(e);
    log.threads.push_back(std::this_thread::get_id());
    if (!r) log.stopFromHandler = r.error();
}

TEST_F(AsioTest, EventsXrunsAreDeliveredOffTheAudioThreadWithCumulativeCounts) {
    EventLog log; log.backend = be.get();
    openOk();
    ASSERT_OK(be->setEventHandler(onEvent, &log));
    ASSERT_OK(be->start());
    for (int i = 0; i < 3; ++i) drv.callbacks()->asioMessage(kAsioOverload, 0, nullptr, nullptr);
    ASSERT_TRUE(waitFor([&] { std::lock_guard l(log.m); return !log.events.empty() && log.events.back().xruns == 3; }));
    {
        std::lock_guard l(log.m);
        std::uint64_t last = 0;
        for (const auto &e : log.events) {
            EXPECT_EQ(e.type, EventType::XRun);
            EXPECT_GE(e.xruns, last);
            last = e.xruns;
        }
        for (const auto &t : log.threads) EXPECT_NE(t, std::this_thread::get_id());
        ASSERT_TRUE(log.stopFromHandler.has_value());
        EXPECT_EQ(*log.stopFromHandler, ErrorType::InvalidState);
    }
}

TEST_F(AsioTest, EventsFailureIsReportedExactlyOnce) {
    EventLog log; log.backend = be.get();
    openOk();
    ASSERT_OK(be->setEventHandler(onEvent, &log));
    ASSERT_OK(be->start());
    drv.callbacks()->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    drv.callbacks()->asioMessage(kAsioBufferSizeChange, 0, nullptr, nullptr);
    drv.callbacks()->sampleRateDidChange(44100.0);
    const auto failures = [&] {
        std::lock_guard l(log.m);
        return std::count_if(log.events.begin(), log.events.end(), [](const Event &e) { return e.type == EventType::Failed; });
    };
    ASSERT_TRUE(waitFor([&] { return failures() >= 1; }));
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(failures(), 1);
    ASSERT_OK(be->stop());
    std::size_t n;
    { std::lock_guard l(log.m); n = log.events.size(); }
    std::this_thread::sleep_for(50ms);
    { std::lock_guard l(log.m); EXPECT_EQ(log.events.size(), n) << "plus d'événement après stop()"; }
}

// ===========================================================================
// 8. Threads et singleton
// ===========================================================================
TEST_F(AsioTest, EveryDriverCallRunsOnOneComStaThreadWhoeverTheCaller) {
    ASSERT_OK(std::async(std::launch::async, [&] { return be->open(makeCfg()); }).get());
    ASSERT_OK(be->setProcessFunction(probeProcess, &probe));
    ASSERT_OK(std::async(std::launch::async, [&] { return be->start(); }).get());
    ASSERT_OK(std::async(std::launch::async, [&] { return be->stop(); }).get());
    ASSERT_OK(std::async(std::launch::async, [&] { return be->close(); }).get());
    const auto threads = drv.callerThreads();
    ASSERT_EQ(threads.size(), 1u) << "tous les appels IASIO doivent partir du même thread";
    EXPECT_NE(*threads.begin(), std::this_thread::get_id());
    EXPECT_TRUE(drv.aptType == APTTYPE_STA || drv.aptType == APTTYPE_MAINSTA) << "apartment " << drv.aptType;
    expectBalanced();
}

TEST_F(AsioTest, OnlyOneInstanceCanHoldTheDriverAtATime) {
    ASIO second;
    openOk();
    ASSERT_ERR(second.open(makeCfg()), ErrorType::EndpointUnavailable);
    ASSERT_ERR(second.start(), ErrorType::InvalidState);
    ASSERT_OK(be->close());
    ASSERT_OK(second.open(makeCfg()));
    ASSERT_ERR(be->open(makeCfg()), ErrorType::EndpointUnavailable);
    ASSERT_OK(second.close());
    ASSERT_OK(be->open(makeCfg()));
    ASSERT_OK(be->close());
    expectBalanced();
}

TEST_F(AsioTest, DestroyingTheHolderFreesTheDriverForOthers) {
    {
        ASIO holder;
        ASSERT_OK(holder.open(makeCfg()));
        ASSERT_ERR(be->open(makeCfg()), ErrorType::EndpointUnavailable);
    }
    ASSERT_OK(be->open(makeCfg()));
}

TEST_F(AsioTest, ConcurrentControlCallsFromManyThreadsStayConsistent) {
    drv.autoPump = true;
    drv.pumpPeriod = 100us;
    ASSERT_OK(be->setProcessFunction(probeProcess, &probe));
    std::atomic<int> go{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(1234 + t);
            while (!go) std::this_thread::yield();
            for (int i = 0; i < 300; ++i) {
                switch (rng() % 5) {
                    case 0: (void)be->open(makeCfg()); break;
                    case 1: (void)be->start(); break;
                    case 2: (void)be->stop(); break;
                    case 3: (void)be->close(); break;
                    default: (void)be->getEndPoints(); (void)be->status(); break;
                }
            }
        });
    }
    go = 1;
    for (auto &t : threads) t.join();

    (void)be->stop();
    (void)be->close();
    ASSERT_OK(be->open(makeCfg()));       // le backend doit être revenu dans un état sain
    ASSERT_OK(be->close());
    expectBalanced();
    EXPECT_EQ(drv.count("start"), drv.count("stop"));
    EXPECT_TRUE(drv.oneThreadPerSession()) << "dans une session, tous les appels IASIO sur le même thread COM";
}

}  // namespace
