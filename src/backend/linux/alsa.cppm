//
// mka.audio.alsa : backend ALSA (mmap, hw:X,Y), révisé pour le temps réel.
//
// Correctifs de la revue :
//  B2  récupération d'xrun : toute erreur (-EPIPE, -ESTRPIPE...) relance le flux
//      (drop -> prepare -> préremplissage -> start) au lieu de boucler à vide ;
//      snd_pcm_avail_update est appelé avant chaque mmap_begin ; les commits courts
//      sont traités comme des xruns.
//  B3  duplex : capture et lecture sont liées (snd_pcm_link) et démarrées ensemble ;
//      on attend d'avoir UNE période complète des deux côtés avant d'appeler le
//      callback (plus aucune frame perdue) ; nombre de périodes fixé ; sw_params
//      (avail_min = période, démarrage manuel) ; préremplissage de la lecture.
//  B4  conversions de format dans mka.audio.convert (Int24 correct, saturation,
//      arrondi, plus de reinterpret_cast).
//  B5  thread audio en SCHED_FIFO (repli sur priorités plus basses), nommé, avec
//      FTZ/DAZ activés. Si le système refuse (pas de droit rtprio), le backend
//      fonctionne quand même : hasRealtimePriority() renvoie alors false.
//  Inclut aussi B1 (userData) et B7 (sortie remise à zéro avant le callback).
//
module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <alsa/asoundlib.h>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

export module mka.audio.backend.alsa;
export import mka.audio.backend;
import mka.audio.constants;
import mka.audio.process;
import mka.audio.convert;

export namespace mka::audio {
    class ALSA final : public Backend {
    public:
        ~ALSA() override {
            if (audioThread_.joinable()) {
                audioThread_.request_stop();
                audioThread_.join();
            }
            closeHandles();
        }

        // Nombre de xruns (over/underruns) récupérés depuis le dernier start().
        [[nodiscard]] std::uint64_t xrunCount() const noexcept {
            return xruns_.load(std::memory_order_relaxed);
        }

        // true si le thread audio a obtenu SCHED_FIFO.
        [[nodiscard]] bool hasRealtimePriority() const noexcept {
            return realtime_.load(std::memory_order_acquire);
        }

        // true si le flux est mort (périphérique disparu, récupération impossible).
        [[nodiscard]] bool hasFailed() const noexcept {
            return failed_.load(std::memory_order_acquire);
        }

    protected:
        [[nodiscard]] std::vector<Endpoint> getEndPoints_() const override {
            std::vector<Endpoint> endpoints;

            try {
                int cardIndex = -1;
                while (snd_card_next(&cardIndex) >= 0 && cardIndex >= 0) {
                    collectCardEndpoints(cardIndex, endpoints);
                }
            } catch (...) {
                return {};
            }

            return endpoints;
        }

        [[nodiscard]] Result open_(EndpointConfig const &endpointCfg) override {
            const bool needCapture = endpointCfg.direction != Direction::Output;
            const bool needPlayback = endpointCfg.direction != Direction::Input;

            try {
                if (needCapture) {
                    if (auto result = openStream(endpointCfg, SND_PCM_STREAM_CAPTURE,
                                                 endpointCfg.inputChannels, capture_); !result) {
                        closeHandles();
                        return result;
                    }
                }

                if (needPlayback) {
                    if (auto result = openStream(endpointCfg, SND_PCM_STREAM_PLAYBACK,
                                                 endpointCfg.outputChannels, playback_); !result) {
                        closeHandles();
                        return result;
                    }
                }

                // B3 : un seul groupe => start/stop/prepare atomiques, même horloge.
                linked_ = needCapture && needPlayback
                          && snd_pcm_link(capture_.pcm, playback_.pcm) == 0;

                config_ = endpointCfg;
                allocateScratchBuffers(endpointCfg, needCapture, needPlayback);
            } catch (...) {
                closeHandles();
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            return {};
        }

        [[nodiscard]] Result start_() override {
            failed_.store(false, std::memory_order_relaxed);
            xruns_.store(0, std::memory_order_relaxed);
            realtime_.store(false, std::memory_order_relaxed);

            Result started;
            try {
                {
                    std::lock_guard lock(threadMutex_);
                    threadReady_ = false;
                    startResult_ = {};
                }

                audioThread_ = std::jthread([this](const std::stop_token &stopToken) {
                    threadMain(stopToken);
                });

                std::unique_lock lock(threadMutex_);
                threadReadyCv_.wait(lock, [this] { return threadReady_; });
                started = startResult_;
            } catch (...) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            // Le thread se termine seul après avoir signalé un échec de démarrage.
            if (!started && audioThread_.joinable()) {
                audioThread_.join();
            }
            return started;
        }

        [[nodiscard]] Result stop_() override {
            if (!audioThread_.joinable()) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            try {
                audioThread_.request_stop();
                audioThread_.join();
            } catch (...) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            // Le prochain start_() refait drop -> prepare -> préremplissage -> start.
            if (capture_) snd_pcm_drop(capture_.pcm);
            if (playback_) snd_pcm_drop(playback_.pcm);

            return {};
        }

        [[nodiscard]] Result close_() override {
            closeHandles();
            return {};
        }

    private:
        // ---------------------------------------------------------------------
        // Types et constantes
        // ---------------------------------------------------------------------

        // Nombre de périodes du buffer ALSA (>= 2 requis ; 3 laisse de la marge
        // au thread audio pour un jitter d'ordonnancement).
        static constexpr unsigned kPeriods = 3;
        // Timeout de snd_pcm_wait : borne aussi la latence de réaction à stop().
        static constexpr int kWaitTimeoutMs = 50;
        // Échecs de récupération consécutifs avant d'abandonner le flux.
        static constexpr int kMaxRecoveryFailures = 20;

        struct Stream {
            snd_pcm_t *pcm = nullptr;
            snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
            convert::Layout layout = convert::Layout::F32;
            snd_pcm_uframes_t periodFrames = 0;
            snd_pcm_uframes_t bufferFrames = 0;
            std::uint32_t channels = 0;

            explicit operator bool() const noexcept { return pcm != nullptr; }
        };

        struct FormatCandidate {
            snd_pcm_format_t alsa;
            convert::Layout layout;
        };

        struct FormatCandidates {
            std::array<FormatCandidate, 2> items{};
            std::size_t count = 0;

            void add(const snd_pcm_format_t alsa, const convert::Layout layout) noexcept {
                items[count++] = FormatCandidate{alsa, layout};
            }
        };

        // ---------------------------------------------------------------------
        // Formats (B4)
        // ---------------------------------------------------------------------

        // Int24 peut exister sous deux formes matérielles : on essaie S24 (32 bits)
        // puis S24_3LE (packed), beaucoup de périphériques USB n'ont que le second.
        static FormatCandidates formatCandidates(const Format format) noexcept {
            FormatCandidates c;
            switch (format) {
                case Format::Int16:
                    c.add(SND_PCM_FORMAT_S16, convert::Layout::S16);
                    break;
                case Format::Int24:
                    c.add(SND_PCM_FORMAT_S24, convert::Layout::S24In32);
                    c.add(SND_PCM_FORMAT_S24_3LE, convert::Layout::S24Packed);
                    break;
                case Format::Int32:
                    c.add(SND_PCM_FORMAT_S32, convert::Layout::S32);
                    break;
                case Format::Float32:
                    c.add(SND_PCM_FORMAT_FLOAT, convert::Layout::F32);
                    break;
                case Format::Float64:
                    c.add(SND_PCM_FORMAT_FLOAT64, convert::Layout::F64);
                    break;
            }
            return c;
        }

        static bool isFormatSupported(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                      const Format format) noexcept {
            const auto candidates = formatCandidates(format);
            for (std::size_t i = 0; i < candidates.count; ++i) {
                if (snd_pcm_hw_params_test_format(pcm, params, candidates.items[i].alsa) == 0) {
                    return true;
                }
            }
            return false;
        }

        // ---------------------------------------------------------------------
        // getEndPoints_
        // ---------------------------------------------------------------------

        static void collectCardEndpoints(const int cardIndex, std::vector<Endpoint> &endpoints) {
            const std::string ctlName = "hw:" + std::to_string(cardIndex);

            snd_ctl_t *ctl = nullptr;
            if (snd_ctl_open(&ctl, ctlName.c_str(), 0) < 0) {
                return;
            }

            snd_ctl_card_info_t *cardInfo = nullptr;
            snd_ctl_card_info_alloca(&cardInfo);

            std::string cardName = ctlName;
            if (snd_ctl_card_info(ctl, cardInfo) >= 0) {
                cardName = snd_ctl_card_info_get_name(cardInfo);
            }

            int deviceIndex = -1;
            while (snd_ctl_pcm_next_device(ctl, &deviceIndex) >= 0 && deviceIndex >= 0) {
                if (auto endpoint = buildDeviceEndpoint(ctl, cardIndex, deviceIndex, cardName)) {
                    endpoints.push_back(std::move(*endpoint));
                }
            }

            snd_ctl_close(ctl);
        }

        static std::optional<Endpoint> buildDeviceEndpoint(snd_ctl_t *ctl, const int cardIndex,
                                                           const int deviceIndex,
                                                           const std::string &cardName) {
            const bool hasPlayback = pcmStreamExists(ctl, deviceIndex, SND_PCM_STREAM_PLAYBACK);
            const bool hasCapture = pcmStreamExists(ctl, deviceIndex, SND_PCM_STREAM_CAPTURE);

            if (!hasPlayback && !hasCapture) {
                return std::nullopt;
            }

            const std::string id = "hw:" + std::to_string(cardIndex) + "," + std::to_string(deviceIndex);
            const std::string pcmName = getPcmDeviceName(
                ctl, deviceIndex, hasPlayback ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE);

            Endpoint endpoint{};
            endpoint.id = id;
            endpoint.name = pcmName.empty() ? cardName : cardName + " - " + pcmName;

            if (hasCapture) {
                endpoint.input = queryStreamCaps(id, SND_PCM_STREAM_CAPTURE);
            }
            if (hasPlayback) {
                endpoint.output = queryStreamCaps(id, SND_PCM_STREAM_PLAYBACK);
            }

            if (!endpoint.input && !endpoint.output) {
                return std::nullopt;
            }

            return endpoint;
        }

        static bool pcmStreamExists(snd_ctl_t *ctl, const int deviceIndex, const snd_pcm_stream_t stream) {
            snd_pcm_info_t *info = nullptr;
            snd_pcm_info_alloca(&info);
            snd_pcm_info_set_device(info, deviceIndex);
            snd_pcm_info_set_subdevice(info, 0);
            snd_pcm_info_set_stream(info, stream);
            return snd_ctl_pcm_info(ctl, info) >= 0;
        }

        static std::string getPcmDeviceName(snd_ctl_t *ctl, const int deviceIndex,
                                            const snd_pcm_stream_t stream) {
            snd_pcm_info_t *info = nullptr;
            snd_pcm_info_alloca(&info);
            snd_pcm_info_set_device(info, deviceIndex);
            snd_pcm_info_set_subdevice(info, 0);
            snd_pcm_info_set_stream(info, stream);

            if (snd_ctl_pcm_info(ctl, info) < 0) {
                return {};
            }

            return snd_pcm_info_get_name(info);
        }

        static std::optional<StreamCapabilities> queryStreamCaps(const std::string &id,
                                                                 const snd_pcm_stream_t stream) {
            snd_pcm_t *pcm = nullptr;
            if (snd_pcm_open(&pcm, id.c_str(), stream, SND_PCM_NONBLOCK) < 0) {
                return std::nullopt;
            }

            snd_pcm_hw_params_t *hwParams = nullptr;
            snd_pcm_hw_params_alloca(&hwParams);

            if (snd_pcm_hw_params_any(pcm, hwParams) < 0) {
                snd_pcm_close(pcm);
                return std::nullopt;
            }

            StreamCapabilities caps{};

            unsigned int minChannels = 0;
            unsigned int maxChannels = 0;
            snd_pcm_hw_params_get_channels_min(hwParams, &minChannels);
            snd_pcm_hw_params_get_channels_max(hwParams, &maxChannels);
            caps.minChannels = minChannels;
            caps.maxChannels = maxChannels;

            for (const auto rate : supportedSampleRates) {
                if (snd_pcm_hw_params_test_rate(pcm, hwParams, rate, 0) == 0) {
                    caps.sampleRates.push_back(rate);
                }
            }

            for (const auto format : supportedFormats) {
                if (isFormatSupported(pcm, hwParams, format)) {
                    caps.formats.push_back(format);
                }
            }

            for (const auto bufferSize : supportedBufferSizes) {
                if (const snd_pcm_uframes_t frames = bufferSize; snd_pcm_hw_params_test_period_size(
                                                                     pcm, hwParams, frames, 0) == 0) {
                    caps.bufferSizes.push_back(bufferSize);
                }
            }

            snd_pcm_close(pcm);

            if (caps.sampleRates.empty() && caps.formats.empty() && caps.bufferSizes.empty()) {
                return std::nullopt;
            }

            return caps;
        }

        // ---------------------------------------------------------------------
        // open_
        // ---------------------------------------------------------------------

        static bool negotiateAccess(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) noexcept {
            return snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_NONINTERLEAVED) == 0
                   || snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0;
        }

        static Result openStream(const EndpointConfig &cfg, const snd_pcm_stream_t direction,
                                 const std::uint32_t channels, Stream &out) noexcept {
            snd_pcm_t *pcm = nullptr;
            if (snd_pcm_open(&pcm, cfg.id.c_str(), direction, 0) < 0) {
                return std::unexpected{ErrorType::EndpointUnavailable};
            }

            const auto fail = [pcm](const ErrorType error) -> Result {
                snd_pcm_close(pcm);
                return std::unexpected{error};
            };

            snd_pcm_hw_params_t *hw = nullptr;
            snd_pcm_hw_params_alloca(&hw);

            if (snd_pcm_hw_params_any(pcm, hw) < 0) return fail(ErrorType::ConfigurationFailed);
            if (!negotiateAccess(pcm, hw)) return fail(ErrorType::ConfigurationFailed);

            // Format : on teste avant de fixer (un set raté peut laisser les params modifiés).
            const auto candidates = formatCandidates(cfg.format);
            std::optional<FormatCandidate> chosen;
            for (std::size_t i = 0; i < candidates.count && !chosen; ++i) {
                if (snd_pcm_hw_params_test_format(pcm, hw, candidates.items[i].alsa) == 0) {
                    chosen = candidates.items[i];
                }
            }
            if (!chosen || snd_pcm_hw_params_set_format(pcm, hw, chosen->alsa) < 0) {
                return fail(ErrorType::FormatNotSupported);
            }

            if (snd_pcm_hw_params_set_channels(pcm, hw, channels) < 0) {
                return fail(ErrorType::ChannelsNotSupported);
            }

            if (snd_pcm_hw_params_set_rate(pcm, hw, cfg.sampleRate, 0) < 0) {
                return fail(ErrorType::SampleRateNotSupported);
            }

            // "bufferSize" de l'API = taille de période (frames par callback).
            if (snd_pcm_hw_params_set_period_size(pcm, hw, cfg.bufferSize, 0) < 0) {
                return fail(ErrorType::BufferSizeNotSupported);
            }

            unsigned int periods = kPeriods;
            int dir = 0;
            if (snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, &dir) < 0) {
                return fail(ErrorType::ConfigurationFailed);
            }

            if (snd_pcm_hw_params(pcm, hw) < 0) return fail(ErrorType::ConfigurationFailed);

            snd_pcm_uframes_t period = 0;
            snd_pcm_uframes_t buffer = 0;
            if (snd_pcm_hw_params_get_period_size(hw, &period, &dir) < 0
                || snd_pcm_hw_params_get_buffer_size(hw, &buffer) < 0
                || period != cfg.bufferSize
                || buffer < 2 * period) {
                return fail(ErrorType::ConfigurationFailed);
            }

            // sw_params : réveil à chaque période, démarrage uniquement manuel
            // (après préremplissage), stop_threshold par défaut (= buffer) pour que
            // l'xrun soit signalé au lieu de rejouer un buffer périmé.
            snd_pcm_sw_params_t *sw = nullptr;
            snd_pcm_sw_params_alloca(&sw);
            snd_pcm_uframes_t boundary = 0;
            if (snd_pcm_sw_params_current(pcm, sw) < 0
                || snd_pcm_sw_params_get_boundary(sw, &boundary) < 0
                || snd_pcm_sw_params_set_avail_min(pcm, sw, period) < 0
                || snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary) < 0
                || snd_pcm_sw_params(pcm, sw) < 0) {
                return fail(ErrorType::ConfigurationFailed);
            }

            if (snd_pcm_prepare(pcm) < 0) return fail(ErrorType::ConfigurationFailed);

            out = Stream{
                .pcm = pcm,
                .format = chosen->alsa,
                .layout = chosen->layout,
                .periodFrames = period,
                .bufferFrames = buffer,
                .channels = channels,
            };
            return {};
        }

        void allocateScratchBuffers(const EndpointConfig &cfg, const bool needCapture,
                                    const bool needPlayback) {
            if (needCapture) {
                inputScratch_.assign(cfg.inputChannels, std::vector<float>(cfg.bufferSize));
                inputChannelPtrs_.resize(cfg.inputChannels);
                for (std::uint32_t i = 0; i < cfg.inputChannels; ++i) {
                    inputChannelPtrs_[i] = inputScratch_[i].data();
                }
            }

            if (needPlayback) {
                outputScratch_.assign(cfg.outputChannels, std::vector<float>(cfg.bufferSize));
                outputChannelPtrs_.resize(cfg.outputChannels);
                for (std::uint32_t i = 0; i < cfg.outputChannels; ++i) {
                    outputChannelPtrs_[i] = outputScratch_[i].data();
                }
            }
        }

        // ---------------------------------------------------------------------
        // Accès mmap (B2, B4)
        // ---------------------------------------------------------------------

        static std::byte *areaPtr(const snd_pcm_channel_area_t &area,
                                  const snd_pcm_uframes_t offset) noexcept {
            const std::size_t bits = area.first + static_cast<std::size_t>(area.step) * offset;
            return static_cast<std::byte *>(area.addr) + bits / 8;
        }

        // Renvoie 0 ou une erreur négative (errno) : -EPIPE = xrun, -ESTRPIPE = suspendu.
        static int readPeriod(const Stream &s, std::vector<std::vector<float> > &dst,
                              const snd_pcm_uframes_t frames) noexcept {
            snd_pcm_uframes_t done = 0;
            while (done < frames) {
                const snd_pcm_channel_area_t *areas = nullptr;
                snd_pcm_uframes_t offset = 0;
                snd_pcm_uframes_t chunk = frames - done;   // peut être réduit (fin de l'anneau)

                if (const int err = snd_pcm_mmap_begin(s.pcm, &areas, &offset, &chunk); err < 0) {
                    return err;
                }

                for (std::uint32_t ch = 0; ch < s.channels; ++ch) {
                    convert::readChannel(s.layout, areaPtr(areas[ch], offset), areas[ch].step / 8,
                                         dst[ch].data() + done, chunk);
                }

                const snd_pcm_sframes_t committed = snd_pcm_mmap_commit(s.pcm, offset, chunk);
                if (committed < 0) return static_cast<int>(committed);
                if (static_cast<snd_pcm_uframes_t>(committed) != chunk) return -EPIPE;

                done += chunk;
            }
            return 0;
        }

        static int writePeriod(const Stream &s, const std::vector<std::vector<float> > &src,
                               const snd_pcm_uframes_t frames) noexcept {
            snd_pcm_uframes_t done = 0;
            while (done < frames) {
                const snd_pcm_channel_area_t *areas = nullptr;
                snd_pcm_uframes_t offset = 0;
                snd_pcm_uframes_t chunk = frames - done;

                if (const int err = snd_pcm_mmap_begin(s.pcm, &areas, &offset, &chunk); err < 0) {
                    return err;
                }

                for (std::uint32_t ch = 0; ch < s.channels; ++ch) {
                    convert::writeChannel(s.layout, areaPtr(areas[ch], offset), areas[ch].step / 8,
                                          src[ch].data() + done, chunk);
                }

                const snd_pcm_sframes_t committed = snd_pcm_mmap_commit(s.pcm, offset, chunk);
                if (committed < 0) return static_cast<int>(committed);
                if (static_cast<snd_pcm_uframes_t>(committed) != chunk) return -EPIPE;

                done += chunk;
            }
            return 0;
        }

        static int writeSilence(const Stream &s, const snd_pcm_uframes_t frames) noexcept {
            if (const snd_pcm_sframes_t avail = snd_pcm_avail_update(s.pcm); avail < 0) {
                return static_cast<int>(avail);
            }

            snd_pcm_uframes_t done = 0;
            while (done < frames) {
                const snd_pcm_channel_area_t *areas = nullptr;
                snd_pcm_uframes_t offset = 0;
                snd_pcm_uframes_t chunk = frames - done;

                if (const int err = snd_pcm_mmap_begin(s.pcm, &areas, &offset, &chunk); err < 0) {
                    return err;
                }

                snd_pcm_areas_silence(areas, offset, s.channels, chunk, s.format);

                const snd_pcm_sframes_t committed = snd_pcm_mmap_commit(s.pcm, offset, chunk);
                if (committed < 0) return static_cast<int>(committed);
                if (static_cast<snd_pcm_uframes_t>(committed) != chunk) return -EPIPE;

                done += chunk;
            }
            return 0;
        }

        // Attend qu'au moins `need` frames soient disponibles (données en capture,
        // place en lecture). Renvoie 0 = prêt, 1 = timeout/arrêt demandé, <0 = erreur.
        static int waitAvail(const Stream &s, const snd_pcm_uframes_t need,
                             const std::stop_token &stopToken) noexcept {
            for (;;) {
                const snd_pcm_sframes_t avail = snd_pcm_avail_update(s.pcm);
                if (avail < 0) return static_cast<int>(avail);
                if (static_cast<snd_pcm_uframes_t>(avail) >= need) return 0;
                if (stopToken.stop_requested()) return 1;

                const int waited = snd_pcm_wait(s.pcm, kWaitTimeoutMs);
                if (waited < 0) return waited;
                if (waited == 0) {
                    // Timeout : si le flux est tombé en xrun/déconnecté sans que le
                    // wait le signale, on le détecte ici plutôt que de boucler.
                    const snd_pcm_state_t state = snd_pcm_state(s.pcm);
                    if (state == SND_PCM_STATE_XRUN) return -EPIPE;
                    if (state == SND_PCM_STATE_DISCONNECTED) return -ENODEV;
                    return 1;
                }
            }
        }

        // ---------------------------------------------------------------------
        // Démarrage et récupération (B2, B3)
        // ---------------------------------------------------------------------

        // Remet les flux dans un état propre, préremplit la lecture avec du silence
        // et démarre tout. Utilisé au start ET après chaque xrun.
        Result beginStreaming() noexcept {
            if (capture_) snd_pcm_drop(capture_.pcm);
            if (playback_) snd_pcm_drop(playback_.pcm);

            if (capture_ && snd_pcm_prepare(capture_.pcm) < 0) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }
            if (playback_ && snd_pcm_prepare(playback_.pcm) < 0) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            if (playback_) {
                // (périodes - 1) de silence : le premier tour de boucle ajoute la dernière.
                const snd_pcm_uframes_t prefill = playback_.bufferFrames - playback_.periodFrames;
                if (writeSilence(playback_, prefill) < 0) {
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }
            }

            if (linked_) {
                // Un start sur un flux lié démarre tout le groupe au même instant.
                snd_pcm_t *leader = capture_ ? capture_.pcm : playback_.pcm;
                if (snd_pcm_start(leader) < 0) {
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }
            } else {
                if (playback_ && snd_pcm_start(playback_.pcm) < 0) {
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }
                if (capture_ && snd_pcm_start(capture_.pcm) < 0) {
                    return std::unexpected{ErrorType::ConfigurationFailed};
                }
            }
            return {};
        }

        bool recover(const int err) noexcept {
            if (err == -ENODEV) return false;   // périphérique disparu : irrécupérable

            if (err == -ESTRPIPE) {
                // Suspend/resume système : reprise standard (se replie sur prepare).
                if (capture_) snd_pcm_recover(capture_.pcm, err, 1);
                if (playback_) snd_pcm_recover(playback_.pcm, err, 1);
            }
            return beginStreaming().has_value();
        }

        // ---------------------------------------------------------------------
        // Thread audio (B5)
        // ---------------------------------------------------------------------

        static bool requestRealtimePriority() noexcept {
            const int maxPriority = sched_get_priority_max(SCHED_FIFO);
            if (maxPriority <= 0) return false;

            // Selon RLIMIT_RTPRIO / rtkit, seules les priorités basses sont permises.
            for (const int wanted : {80, 50, 20, 10, 1}) {
                sched_param param{};
                param.sched_priority = std::min(wanted, maxPriority);
                if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
                    return true;
                }
            }
            return false;
        }

        static void flushDenormals() noexcept {
#if defined(__SSE__)
            _mm_setcsr(_mm_getcsr() | 0x8040u);   // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__)
            std::uint64_t fpcr = 0;
            asm volatile("mrs %0, fpcr" : "=r"(fpcr));
            fpcr |= (std::uint64_t{1} << 24);     // FZ
            asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
        }

        void threadMain(const std::stop_token &stopToken) noexcept {
            pthread_setname_np(pthread_self(), "mka-alsa");
            realtime_.store(requestRealtimePriority(), std::memory_order_release);
            flushDenormals();

            const Result started = beginStreaming();
            {
                std::lock_guard lock(threadMutex_);
                startResult_ = started;
                threadReady_ = true;
            }
            threadReadyCv_.notify_one();

            if (started) {
                audioLoop(stopToken);
            }
        }

        // Un cycle = une période complète en entrée ET en sortie.
        // Renvoie 0 = cycle traité, 1 = rien à faire (timeout), <0 = erreur ALSA.
        int runCycle(const std::stop_token &stopToken) noexcept {
            const snd_pcm_uframes_t period = config_.bufferSize;

            if (capture_) {
                if (const int r = waitAvail(capture_, period, stopToken); r != 0) return r;
            }
            if (playback_) {
                if (const int r = waitAvail(playback_, period, stopToken); r != 0) return r;
            }

            AudioProcessContext ctx{};
            ctx.frames = static_cast<std::uint32_t>(period);

            if (capture_) {
                if (const int r = readPeriod(capture_, inputScratch_, period); r < 0) return r;
                ctx.input.channels = inputChannelPtrs_.data();
                ctx.input.count = static_cast<std::uint32_t>(inputChannelPtrs_.size());
            }

            if (playback_) {
                // B7 : le callback reçoit toujours une sortie à zéro.
                for (auto &channel : outputScratch_) {
                    std::fill(channel.begin(), channel.end(), 0.0f);
                }
                ctx.output.channels = outputChannelPtrs_.data();
                ctx.output.count = static_cast<std::uint32_t>(outputChannelPtrs_.size());
            }

            if (callback != nullptr) {
                callback(userData, ctx);
            }

            if (playback_) {
                if (const int r = writePeriod(playback_, outputScratch_, period); r < 0) return r;
            }
            return 0;
        }

        void audioLoop(const std::stop_token &stopToken) noexcept {
            int consecutiveFailures = 0;

            while (!stopToken.stop_requested()) {
                const int result = runCycle(stopToken);
                if (result >= 0) {
                    consecutiveFailures = 0;
                    continue;
                }

                if (stopToken.stop_requested()) break;

                xruns_.fetch_add(1, std::memory_order_relaxed);
                if (recover(result)) {
                    consecutiveFailures = 0;
                    continue;
                }

                if (++consecutiveFailures >= kMaxRecoveryFailures || result == -ENODEV) {
                    failed_.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // ---------------------------------------------------------------------
        // close_ / destructeur
        // ---------------------------------------------------------------------

        void closeHandles() noexcept {
            if (linked_ && capture_) {
                snd_pcm_unlink(capture_.pcm);
            }
            linked_ = false;

            if (capture_) {
                snd_pcm_close(capture_.pcm);
                capture_ = Stream{};
            }
            if (playback_) {
                snd_pcm_close(playback_.pcm);
                playback_ = Stream{};
            }
        }

        Stream capture_;
        Stream playback_;
        bool linked_ = false;
        EndpointConfig config_{};

        std::vector<std::vector<float> > inputScratch_;
        std::vector<std::vector<float> > outputScratch_;
        std::vector<float *> inputChannelPtrs_;
        std::vector<float *> outputChannelPtrs_;

        std::jthread audioThread_;
        std::mutex threadMutex_;
        std::condition_variable threadReadyCv_;
        bool threadReady_ = false;
        Result startResult_;

        std::atomic<std::uint64_t> xruns_{0};
        std::atomic<bool> realtime_{false};
        std::atomic<bool> failed_{false};
    };
}
