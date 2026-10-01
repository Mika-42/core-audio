//
// mka.audio.backend.asio : backend ASIO (Windows), calqué sur mka.audio.backend.alsa.
//
// Dépendances de build :
//  - SDK ASIO de Steinberg, EN-TÊTES SEULEMENT (asio.h, asiosys.h, iasiodrv.h).
//    On parle directement à l'interface COM IASIO : ni asio.cpp, ni asiodrivers.cpp,
//    ni asiolist.cpp (leur état global et leurs tampons char[] ne sont pas nécessaires).
//    Cible x64. Pour du x86 avec MinGW, il faudrait en plus iasiothiscallresolver.
//  - Bibliothèques : ole32, advapi32, user32.
//
// Contrat des hooks :
//  open_         COM + IASIO::init, vérifie canaux / cadence / taille / format, createBuffers.
//                Erreurs typées (EndpointUnavailable, ChannelsNotSupported, ...).
//  start_        IASIO::start (bloque l'appelant). Le thread audio est créé PAR LE DRIVER et
//                appelle bufferSwitch : il est forcément différent du thread appelant.
//  stop_         IASIO::stop, puis attente que plus aucun callback ne soit en cours.
//  close_        disposeBuffers + Release du driver.
//  getEndPoints_ balayage de HKLM\SOFTWARE\ASIO + sondage de chaque driver. Ne lève jamais.
//
// Particularités d'ASIO par rapport à ALSA :
//  1. Le driver est global au processus : une seule instance de ce backend peut avoir un
//     endpoint ouvert à la fois (sinon EndpointUnavailable). Pendant ce temps,
//     getEndPoints_ ne recharge aucun driver et renvoie le dernier balayage.
//  2. Les drivers sont des objets COM, souvent STA, parfois avec des fenêtres cachées :
//     tous les appels IASIO (hors callbacks) passent par UN thread dédié (STA + pompe de
//     messages), quel que soit le thread qui appelle open/start/stop/close.
//  3. Le thread audio appartient au driver : realtime reste Unknown, on ne le renomme pas,
//     et FTZ/DAZ sont activés le temps du callback puis restaurés.
//  4. Le format n'est pas négociable : c'est celui du driver. EndpointConfig::format doit
//     correspondre, sinon FormatNotSupported (les caps n'annoncent que les formats natifs).
//  5. Entrées et sorties arrivent dans le même bufferSwitch : pas de lien de flux à gérer.
//
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif
#include "iasiodrv.h"

export module mka.audio.backend.asio;
export import mka.audio.backend.abstract;
import mka.audio.constants;
import mka.audio.process;
import mka.audio.convert;

namespace mka::audio::asio_detail {

    // ---------------------------------------------------------------------
    // Conversions de chaînes et registre
    // ---------------------------------------------------------------------

    inline std::string toUtf8(const std::wstring &wide) {
        if (wide.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string out(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            out.data(), size, nullptr, nullptr);
        return out;
    }

    inline std::wstring toWide(const std::string &utf8) {
        if (utf8.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                             nullptr, 0);
        if (size <= 0) return {};
        std::wstring out(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
        return out;
    }

    inline std::wstring readRegString(const wchar_t *path, const wchar_t *value) {
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, path, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes)
            != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
            return {};
        }
        std::wstring text(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, path, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes)
            != ERROR_SUCCESS) {
            return {};
        }
        while (!text.empty() && text.back() == L'\0') text.pop_back();
        return text;
    }

    inline std::wstring readDriverValue(const std::wstring &key, const wchar_t *value) {
        const std::wstring path = L"SOFTWARE\\ASIO\\" + key;
        return readRegString(path.c_str(), value);
    }

    struct DriverEntry {
        std::wstring key;           // nom de la sous-clé = identifiant de l'endpoint
        std::wstring description;
    };

    // Les drivers ASIO s'enregistrent sous HKLM\SOFTWARE\ASIO\<nom> (valeur CLSID).
    // Un processus 64 bits voit les drivers 64 bits, un 32 bits les drivers 32 bits.
    inline std::vector<DriverEntry> listDrivers() {
        std::vector<DriverEntry> drivers;

        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) {
            return drivers;
        }

        for (DWORD index = 0;; ++index) {
            wchar_t name[256];
            DWORD length = 256;
            const LONG status = RegEnumKeyExW(root, index, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) continue;

            DriverEntry entry;
            entry.key.assign(name, length);
            entry.description = readDriverValue(entry.key, L"Description");
            drivers.push_back(std::move(entry));
        }

        RegCloseKey(root);
        return drivers;
    }

    // ---------------------------------------------------------------------
    // Thread COM dédié (STA + pompe de messages)
    // ---------------------------------------------------------------------

    constexpr UINT kRunTask = WM_APP + 1;

    class ComThread {
    public:
        ComThread() = default;
        ~ComThread() { stop(); }

        ComThread(const ComThread &) = delete;
        ComThread &operator=(const ComThread &) = delete;

        [[nodiscard]] bool running() const noexcept {
            return threadId_.load(std::memory_order_acquire) != 0;
        }

        // Démarre le thread et attend que COM et sa file de messages soient prêts.
        [[nodiscard]] bool start() {
            if (running()) return true;

            std::unique_lock lock(mutex_);
            ready_ = false;
            thread_ = std::thread([this] { threadMain(); });
            cv_.wait(lock, [this] { return ready_; });

            if (threadId_.load(std::memory_order_acquire) == 0) {   // CoInitializeEx a échoué
                lock.unlock();
                thread_.join();
                return false;
            }
            return true;
        }

        void stop() noexcept {
            try {
                if (!thread_.joinable()) return;
                if (const DWORD id = threadId_.load(std::memory_order_acquire); id != 0) {
                    PostThreadMessageW(id, WM_QUIT, 0, 0);
                }
                thread_.join();
            } catch (...) {
            }
            threadId_.store(0, std::memory_order_release);
        }

        // Exécute fn() sur le thread COM et attend son résultat.
        template <class F>
        [[nodiscard]] Result run(F &&fn) noexcept {
            try {
                Result out = std::unexpected{ErrorType::ConfigurationFailed};
                std::mutex m;
                std::condition_variable cv;
                bool done = false;

                std::function<void()> task = [&] {
                    try {
                        out = fn();
                    } catch (...) {
                        out = std::unexpected{ErrorType::ConfigurationFailed};
                    }
                    std::lock_guard lock(m);
                    done = true;
                    cv.notify_one();   // sous verrou : m et cv restent valides pour l'appelant
                };

                const DWORD id = threadId_.load(std::memory_order_acquire);
                if (id == 0 || !PostThreadMessageW(id, kRunTask, 0, reinterpret_cast<LPARAM>(&task))) {
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }

                std::unique_lock lock(m);
                cv.wait(lock, [&] { return done; });
                return out;
            } catch (...) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }
        }

    private:
        void threadMain() noexcept {
            const bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

            MSG msg{};
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // crée la file de messages

            {
                std::lock_guard lock(mutex_);
                threadId_.store(comOk ? GetCurrentThreadId() : 0, std::memory_order_release);
                ready_ = true;
                cv_.notify_one();
            }
            if (!comOk) return;

            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.hwnd == nullptr && msg.message == kRunTask) {
                    (*reinterpret_cast<std::function<void()> *>(msg.lParam))();
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            CoUninitialize();
        }

        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool ready_ = false;
        std::atomic<DWORD> threadId_{0};
    };

    // ---------------------------------------------------------------------
    // Driver IASIO (RAII). load() et reset() doivent tourner sur le thread COM.
    // ---------------------------------------------------------------------

    class Driver {
    public:
        Driver() noexcept = default;
        ~Driver() { reset(); }

        Driver(const Driver &) = delete;
        Driver &operator=(const Driver &) = delete;

        IASIO *operator->() const noexcept { return drv_; }
        explicit operator bool() const noexcept { return drv_ != nullptr; }

        bool load(const std::wstring &key) noexcept {
            reset();
            try {
                const std::wstring text = readDriverValue(key, L"CLSID");
                CLSID clsid{};
                if (text.empty() || FAILED(CLSIDFromString(text.c_str(), &clsid))) return false;

                // Convention ASIO : le CLSID du driver sert aussi d'IID.
                IASIO *raw = nullptr;
                if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                            reinterpret_cast<void **>(&raw))) || raw == nullptr) {
                    return false;
                }
                if (raw->init(GetDesktopWindow()) != ASIOTrue) {
                    raw->Release();
                    return false;
                }
                drv_ = raw;
                return true;
            } catch (...) {
                return false;
            }
        }

        void reset() noexcept {
            if (drv_ != nullptr) {
                drv_->Release();
                drv_ = nullptr;
            }
        }

    private:
        IASIO *drv_ = nullptr;
    };

    // ---------------------------------------------------------------------
    // Formats et tailles de buffer
    // ---------------------------------------------------------------------

    struct NativeFormat {
        Format format;
        convert::Layout layout;
    };

    // Seuls les formats little-endian que mka.audio.convert sait traiter sont gérés.
    // Non gérés : Int32LSB16/18/20 (à ajouter à convert), variantes MSB, DSD.
    inline std::optional<NativeFormat> nativeFormat(const ASIOSampleType type) noexcept {
        switch (type) {
            case ASIOSTInt16LSB:   return NativeFormat{Format::Int16, convert::Layout::S16};
            case ASIOSTInt24LSB:   return NativeFormat{Format::Int24, convert::Layout::S24Packed};
            case ASIOSTInt32LSB24: return NativeFormat{Format::Int24, convert::Layout::S24In32};
            case ASIOSTInt32LSB:   return NativeFormat{Format::Int32, convert::Layout::S32};
            case ASIOSTFloat32LSB: return NativeFormat{Format::Float32, convert::Layout::F32};
            case ASIOSTFloat64LSB: return NativeFormat{Format::Float64, convert::Layout::F64};
            default:               return std::nullopt;
        }
    }

    // granularité -1 : puissances de deux ; > 0 : minSize + k * granularité ; 0 : taille unique.
    inline bool isBufferSizeValid(const long size, const long minSize, const long maxSize,
                                  const long preferred, const long granularity) noexcept {
        if (size <= 0 || size < minSize || size > maxSize) return false;
        if (minSize == maxSize) return size == minSize;
        if (granularity == -1) return std::has_single_bit(static_cast<unsigned long>(size));
        if (granularity > 0) return (size - minSize) % granularity == 0;
        return size == preferred;
    }

    // ---------------------------------------------------------------------
    // Sondage d'un driver pour getEndPoints_ (sur le thread COM)
    // ---------------------------------------------------------------------

    inline std::optional<Endpoint> probeDriver(const DriverEntry &entry) {
        Driver drv;
        if (!drv.load(entry.key)) return std::nullopt;

        long numIn = 0;
        long numOut = 0;
        if (drv->getChannels(&numIn, &numOut) != ASE_OK) return std::nullopt;

        long minSize = 0;
        long maxSize = 0;
        long preferred = 0;
        long granularity = 0;
        if (drv->getBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK) {
            return std::nullopt;
        }

        std::vector<SampleRate> rates;
        for (const auto rate : supportedSampleRates) {
            if (drv->canSampleRate(static_cast<ASIOSampleRate>(rate)) == ASE_OK) {
                rates.push_back(rate);
            }
        }

        std::vector<BufferSize> sizes;
        for (const auto size : supportedBufferSizes) {
            if (isBufferSizeValid(static_cast<long>(size), minSize, maxSize, preferred, granularity)) {
                sizes.push_back(size);
            }
        }

        const auto makeCaps = [&](const bool isInput, const long count) -> std::optional<StreamCapabilities> {
            if (count <= 0) return std::nullopt;

            StreamCapabilities caps{};
            caps.minChannels = 1;
            caps.maxChannels = static_cast<std::uint32_t>(count);
            caps.sampleRates = rates;
            caps.bufferSizes = sizes;

            for (long channel = 0; channel < count; ++channel) {
                ASIOChannelInfo info{};
                info.channel = channel;
                info.isInput = isInput ? ASIOTrue : ASIOFalse;
                if (drv->getChannelInfo(&info) != ASE_OK) continue;

                if (const auto native = nativeFormat(info.type)) {
                    if (std::ranges::find(caps.formats, native->format) == caps.formats.end()) {
                        caps.formats.push_back(native->format);
                    }
                }
            }

            if (caps.sampleRates.empty() && caps.formats.empty() && caps.bufferSizes.empty()) {
                return std::nullopt;
            }
            return caps;
        };

        Endpoint endpoint{};
        endpoint.id = toUtf8(entry.key);
        endpoint.name = entry.description.empty() ? endpoint.id : toUtf8(entry.description);
        endpoint.input = makeCaps(true, numIn);
        endpoint.output = makeCaps(false, numOut);

        if (!endpoint.input && !endpoint.output) return std::nullopt;
        return endpoint;
    }

    // ---------------------------------------------------------------------
    // Protections du callback
    // ---------------------------------------------------------------------

    // Compte les callbacks en cours : stop_/close_ attendent qu'il retombe à zéro
    // avant de toucher aux buffers (ASIOStop ne garantit pas toujours cette attente).
    struct CallbackScope {
        std::atomic<std::uint32_t> &counter;
        explicit CallbackScope(std::atomic<std::uint32_t> &c) noexcept : counter(c) {
            counter.fetch_add(1);
        }
        ~CallbackScope() { counter.fetch_sub(1); }
    };

    // Le thread appartient au driver : on active FTZ/DAZ le temps du callback, puis on
    // restaure le registre. (ARM64 : non traité, le guard est alors un no-op.)
    struct DenormalGuard {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        unsigned int saved = _mm_getcsr();
        DenormalGuard() noexcept { _mm_setcsr(saved | 0x8040u); }   // FTZ (bit 15) | DAZ (bit 6)
        ~DenormalGuard() { _mm_setcsr(saved); }
#else
        DenormalGuard() noexcept {}
        ~DenormalGuard() {}
#endif
    };
}

export namespace mka::audio {
    class ASIO final : public Backend {
    public:
        ~ASIO() override {
            if (streaming_ && com_.running()) {
                (void)com_.run([this]() -> Result {
                    (void)driver_->stop();
                    return {};
                });
            }
            (void)teardown();
        }

    protected:
        [[nodiscard]] std::vector<Endpoint> getEndPoints_() const override {
            try {
                // ASIO est global au processus : pendant qu'un endpoint est ouvert, recharger
                // un autre driver couperait le flux. On renvoie alors le dernier balayage.
                std::scoped_lock lock(globalMutex_);
                if (instance_.load(std::memory_order_acquire) != nullptr) {
                    return catalogCache_;
                }

                asio_detail::ComThread com;
                if (!com.start()) return catalogCache_;

                std::vector<Endpoint> endpoints;
                for (const auto &entry : asio_detail::listDrivers()) {
                    std::optional<Endpoint> endpoint;
                    (void)com.run([&]() -> Result {
                        endpoint = asio_detail::probeDriver(entry);
                        return {};
                    });
                    if (endpoint) endpoints.push_back(std::move(*endpoint));
                }

                catalogCache_ = endpoints;
                return endpoints;
            } catch (...) {
                return {};
            }
        }

        [[nodiscard]] Result open_(EndpointConfig const &endpointCfg) override {
            const bool needCapture = endpointCfg.direction != Direction::Output;
            const bool needPlayback = endpointCfg.direction != Direction::Input;

            try {
                {
                    std::scoped_lock lock(globalMutex_);
                    ASIO *expected = nullptr;
                    if (!instance_.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
                        return std::unexpected{ErrorType::EndpointUnavailable};
                    }
                }

                // Avant tout appel au driver : les callbacks lisent config_.
                config_ = endpointCfg;

                if (!com_.start()) {
                    (void)teardown();
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }

                const Result opened = com_.run([&]() -> Result {
                    return openOnComThread(endpointCfg, needCapture, needPlayback);
                });
                if (!opened) {
                    (void)teardown();
                    return opened;
                }
            } catch (...) {
                (void)teardown();
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            return {};
        }

        [[nodiscard]] Result start_() override {
            // Avant ASIOStart : certains drivers appellent bufferSwitch avant son retour.
            accepting_.store(true);

            const Result started = com_.run([this]() -> Result {
                if (driver_->start() != ASE_OK) return std::unexpected{ErrorType::ConfigurationFailed};
                return {};
            });

            if (!started) {
                accepting_.store(false);
                (void)waitCallbacksDrained();
                return started;
            }

            streaming_ = true;
            return {};
        }

        [[nodiscard]] Result stop_() override {
            const Result stopped = com_.run([this]() -> Result {
                if (driver_->stop() != ASE_OK) return std::unexpected{ErrorType::ConfigurationFailed};
                return {};
            });
            if (!stopped) return stopped;

            streaming_ = false;
            accepting_.store(false);
            if (!waitCallbacksDrained()) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }
            return {};
        }

        [[nodiscard]] Result close_() override {
            return teardown();
        }

    private:
        using Layout = convert::Layout;

        // ---------------------------------------------------------------------
        // open_ (sur le thread COM)
        // ---------------------------------------------------------------------

        Result openOnComThread(const EndpointConfig &cfg, const bool needCapture, const bool needPlayback) {
            const std::wstring key = asio_detail::toWide(cfg.id);
            // Le nom est utilisé dans un chemin de registre : pas de séparateur.
            if (key.empty() || key.find(L'\\') != std::wstring::npos || !driver_.load(key)) {
                return std::unexpected{ErrorType::EndpointUnavailable};
            }

            long maxIn = 0;
            long maxOut = 0;
            if (driver_->getChannels(&maxIn, &maxOut) != ASE_OK) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            const long wantIn = needCapture ? static_cast<long>(cfg.inputChannels) : 0;
            const long wantOut = needPlayback ? static_cast<long>(cfg.outputChannels) : 0;
            if ((needCapture && (wantIn < 1 || wantIn > maxIn))
                || (needPlayback && (wantOut < 1 || wantOut > maxOut))) {
                return std::unexpected{ErrorType::ChannelsNotSupported};
            }

            const auto rate = static_cast<ASIOSampleRate>(cfg.sampleRate);
            ASIOSampleRate current = 0;
            if (driver_->canSampleRate(rate) != ASE_OK) {
                return std::unexpected{ErrorType::SampleRateNotSupported};
            }
            if (driver_->getSampleRate(&current) != ASE_OK || current != rate) {
                if (driver_->setSampleRate(rate) != ASE_OK) {
                    return std::unexpected{ErrorType::SampleRateNotSupported};
                }
            }

            long minSize = 0;
            long maxSize = 0;
            long preferred = 0;
            long granularity = 0;
            if (driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }
            // "bufferSize" de l'API = frames par callback = taille de buffer ASIO.
            if (!asio_detail::isBufferSizeValid(static_cast<long>(cfg.bufferSize), minSize, maxSize,
                                                preferred, granularity)) {
                return std::unexpected{ErrorType::BufferSizeNotSupported};
            }

            infos_.clear();
            inLayouts_.clear();
            outLayouts_.clear();
            if (auto result = describeChannels(true, wantIn, cfg.format, inLayouts_); !result) return result;
            if (auto result = describeChannels(false, wantOut, cfg.format, outLayouts_); !result) return result;

            if (driver_->createBuffers(infos_.data(), static_cast<long>(infos_.size()),
                                       static_cast<long>(cfg.bufferSize), callbacks()) != ASE_OK) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }
            buffersCreated_ = true;

            outputReady_ = driver_->future(kAsioCanOutputReady, nullptr) == ASE_SUCCESS;

            inCount_ = static_cast<std::uint32_t>(wantIn);
            outCount_ = static_cast<std::uint32_t>(wantOut);
            frames_ = cfg.bufferSize;
            allocateScratchBuffers();
            return {};
        }

        // Vérifie que chaque canal actif a un format natif égal à celui demandé, et
        // prépare les ASIOBufferInfo (entrées d'abord, puis sorties).
        Result describeChannels(const bool isInput, const long count, const Format format,
                                std::vector<Layout> &layouts) {
            for (long channel = 0; channel < count; ++channel) {
                ASIOChannelInfo info{};
                info.channel = channel;
                info.isInput = isInput ? ASIOTrue : ASIOFalse;
                if (driver_->getChannelInfo(&info) != ASE_OK) {
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }

                const auto native = asio_detail::nativeFormat(info.type);
                if (!native || native->format != format) {
                    return std::unexpected{ErrorType::FormatNotSupported};
                }
                layouts.push_back(native->layout);

                ASIOBufferInfo bufferInfo{};
                bufferInfo.isInput = info.isInput;
                bufferInfo.channelNum = channel;
                infos_.push_back(bufferInfo);
            }
            return {};
        }

        void allocateScratchBuffers() {
            inputScratch_.assign(inCount_, std::vector<float>(frames_));
            inputChannelPtrs_.resize(inCount_);
            for (std::uint32_t i = 0; i < inCount_; ++i) {
                inputChannelPtrs_[i] = inputScratch_[i].data();
            }

            outputScratch_.assign(outCount_, std::vector<float>(frames_));
            outputChannelPtrs_.resize(outCount_);
            for (std::uint32_t i = 0; i < outCount_; ++i) {
                outputChannelPtrs_[i] = outputScratch_[i].data();
            }
        }

        // ---------------------------------------------------------------------
        // Callbacks ASIO (thread du driver, temps réel)
        // ---------------------------------------------------------------------

        // ASIO n'a pas de pointeur utilisateur dans ses callbacks : instance unique globale.
        static inline std::atomic<ASIO *> instance_{nullptr};

        static void bufferSwitchCb(const long index, ASIOBool) noexcept {
            if (ASIO *self = instance_.load(std::memory_order_acquire)) self->onBuffer(index);
        }

        static ASIOTime *bufferSwitchTimeInfoCb(ASIOTime *, const long index, ASIOBool) noexcept {
            bufferSwitchCb(index, ASIOFalse);
            return nullptr;
        }

        static void sampleRateDidChangeCb(const ASIOSampleRate rate) noexcept {
            ASIO *self = instance_.load(std::memory_order_acquire);
            if (self != nullptr && std::llround(rate) != static_cast<long long>(self->config_.sampleRate)) {
                self->notifyFailed();   // horloge perdue (0) ou cadence changée depuis le panneau
            }
        }

        static long asioMessageCb(const long selector, const long value, void *, double *) noexcept {
            switch (selector) {
                case kAsioSelectorSupported:
                    switch (value) {
                        case kAsioEngineVersion:
                        case kAsioResetRequest:
                        case kAsioBufferSizeChange:
                        case kAsioResyncRequest:
                        case kAsioLatenciesChanged:
                        case kAsioOverload:          // SDK >= 2.3
                            return 1;
                        default:
                            return 0;
                    }
                case kAsioEngineVersion:
                    return 2;
                case kAsioSupportsTimeInfo:
                    return 0;   // on reste sur bufferSwitch simple
                case kAsioResetRequest:
                case kAsioBufferSizeChange:
                    // Le flux n'est plus valable : impossible de recharger depuis ce thread.
                    // On le signale ; l'application fait stop() puis close() (EventType::Failed).
                    if (ASIO *self = instance_.load(std::memory_order_acquire)) self->notifyFailed();
                    return 1;
                case kAsioResyncRequest:
                case kAsioOverload:
                    if (ASIO *self = instance_.load(std::memory_order_acquire)) self->notifyXRun();
                    return 1;
                case kAsioLatenciesChanged:
                    return 1;
                default:
                    return 0;
            }
        }

        // Le driver garde ce pointeur : durée de vie statique.
        static ASIOCallbacks *callbacks() noexcept {
            static ASIOCallbacks table{
                &bufferSwitchCb,
                &sampleRateDidChangeCb,
                &asioMessageCb,
                &bufferSwitchTimeInfoCb,
            };
            return &table;
        }

        // Un appel = une période complète en entrée ET en sortie (même horloge).
        void onBuffer(const long index) noexcept {
            asio_detail::CallbackScope scope{inFlight_};
            if (!accepting_.load() || (index != 0 && index != 1)) return;

            asio_detail::DenormalGuard denormals;

            AudioProcessContext ctx{};
            ctx.frames = frames_;

            if (inCount_ > 0) {
                for (std::uint32_t ch = 0; ch < inCount_; ++ch) {
                    const Layout layout = inLayouts_[ch];
                    convert::readChannel(layout, static_cast<const std::byte *>(infos_[ch].buffers[index]),
                                         convert::bytesPerSample(layout), inputScratch_[ch].data(), frames_);
                }
                ctx.input.channels = inputChannelPtrs_.data();
                ctx.input.count = inCount_;
            }

            if (outCount_ > 0) {
                // Le callback reçoit toujours une sortie à zéro.
                for (auto &channel : outputScratch_) {
                    std::fill(channel.begin(), channel.end(), 0.0f);
                }
                ctx.output.channels = outputChannelPtrs_.data();
                ctx.output.count = outCount_;
            }

            if (callback != nullptr) {
                callback(userData, ctx);
            }

            for (std::uint32_t ch = 0; ch < outCount_; ++ch) {
                const Layout layout = outLayouts_[ch];
                convert::writeChannel(layout, static_cast<std::byte *>(infos_[inCount_ + ch].buffers[index]),
                                      convert::bytesPerSample(layout), outputScratch_[ch].data(), frames_);
            }

            if (outputReady_) {
                (void)driver_->outputReady();
            }
        }

        // ---------------------------------------------------------------------
        // Arrêt / fermeture
        // ---------------------------------------------------------------------

        // Attend (borné à 2 s) qu'aucun callback ne soit en cours.
        bool waitCallbacksDrained() const noexcept {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (inFlight_.load() != 0) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        // Libère tout, même partiellement ouvert. Renvoie une erreur si disposeBuffers échoue.
        Result teardown() noexcept {
            Result result;

            accepting_.store(false);
            (void)waitCallbacksDrained();

            if (com_.running()) {
                result = com_.run([this]() -> Result {
                    Result r;
                    if (buffersCreated_) {
                        if (driver_->disposeBuffers() != ASE_OK) {
                            r = std::unexpected{ErrorType::ConfigurationFailed};
                        }
                        buffersCreated_ = false;
                    }
                    driver_.reset();   // Release sur le thread COM qui l'a créé
                    return r;
                });
            }
            com_.stop();

            ASIO *self = this;
            instance_.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);

            streaming_ = false;
            outputReady_ = false;
            inCount_ = 0;
            outCount_ = 0;
            infos_.clear();
            inLayouts_.clear();
            outLayouts_.clear();
            inputScratch_.clear();
            outputScratch_.clear();
            inputChannelPtrs_.clear();
            outputChannelPtrs_.clear();
            return result;
        }

        // ---------------------------------------------------------------------
        // État
        // ---------------------------------------------------------------------

        // Protège l'accès à instance_ et au cache du catalogue (ASIO est global au processus).
        static inline std::mutex globalMutex_;
        static inline std::vector<Endpoint> catalogCache_;

        asio_detail::ComThread com_;
        asio_detail::Driver driver_;

        EndpointConfig config_{};
        std::vector<ASIOBufferInfo> infos_;   // doit rester valide jusqu'à disposeBuffers
        std::vector<Layout> inLayouts_;
        std::vector<Layout> outLayouts_;
        std::uint32_t inCount_ = 0;
        std::uint32_t outCount_ = 0;
        std::uint32_t frames_ = 0;
        bool buffersCreated_ = false;
        bool outputReady_ = false;
        bool streaming_ = false;

        std::vector<std::vector<float> > inputScratch_;
        std::vector<std::vector<float> > outputScratch_;
        std::vector<float *> inputChannelPtrs_;
        std::vector<float *> outputChannelPtrs_;

        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);

        // Accédés par le thread du driver : sur leur propre ligne de cache.
        alignas(64) std::atomic<bool> accepting_{false};
        std::atomic<std::uint32_t> inFlight_{0};
    };
}
