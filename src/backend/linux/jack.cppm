//
// mka.audio.jack — Backend JACK (implémentation KISS/YAGNI)
//
// Particularités de JACK qui guident la conception :
//  - Le serveur impose sample rate et taille de buffer, et les ports sont
//    toujours du float32 mono non entrelacé : ça correspond exactement à
//    InputBuffer/OutputBuffer. Le backend ne négocie donc rien, il *valide*
//    que EndpointConfig correspond au serveur (SampleRateNotSupported /
//    BufferSizeNotSupported sinon) et n'accepte que Format::Float32.
//  - Un JACK client est naturellement full-duplex : Direction::Duplex est
//    géré, avec un seul callback recevant entrées et sorties ensemble.
//  - Un "endpoint" est un client JACK du graphe (ex. "system"). Ses ports de
//    sortie sont nos sources (capacité input), ses ports d'entrée nos
//    destinations (capacité output). Id = nom du client, sans le ":port".
//  - Id vide : le client enregistre ses ports sans les connecter (routage
//    laissé à l'utilisateur, ex. qjackctl / pw-jack).
//  - open_ enregistre les ports ; start_ = jack_activate + connexions (les
//    connexions étant retirées par jack_deactivate, elles sont refaites à
//    chaque start) ; stop_ = jack_deactivate ; close_ = jack_client_close.
//  - Le serveur n'est jamais lancé automatiquement (JackNoStartServer).
//  - Non géré (YAGNI) : callback de shutdown serveur, xruns, changement
//    dynamique de rate/buffer.
//
module;
#include <jack/jack.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module mka.audio.jack;

import mka.audio.backend;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;
import mka.audio.process;

namespace mka::audio {

    namespace {
        using ClientPtr = std::unique_ptr<jack_client_t, decltype(&jack_client_close)>;
        using PortList = std::unique_ptr<const char *, decltype(&jack_free)>;

        // Noms complets ("client:port") des ports audio du client `clientName`
        // possédant tous les `flags` demandés.
        std::vector<std::string> portsOf(jack_client_t *client, const std::string_view clientName,
                                         const unsigned long flags) {
            std::vector<std::string> result;
            const PortList ports{jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, flags), &jack_free};
            if (!ports) return result;

            const std::string prefix = std::string{clientName} + ":";
            for (const char **p = ports.get(); *p; ++p) {
                if (std::string_view{*p}.starts_with(prefix)) result.emplace_back(*p);
            }
            return result;
        }
    }

    export class JACK final : public Backend {
        public:
            JACK() noexcept = default;

            ~JACK() override {
                teardown();
            }

        protected:
            [[nodiscard]] std::vector<Endpoint> getEndPoints_() const override;
            [[nodiscard]] Result open_(EndpointConfig const &endpointCfg) override;
            [[nodiscard]] Result start_() override;
            [[nodiscard]] Result stop_() override;
            [[nodiscard]] Result close_() override;

        private:
            static int onProcess(jack_nframes_t nframes, void *arg);

            // Ferme le client (ce qui le désactive et désenregistre ses ports).
            // Renvoie le code de jack_client_close.
            int teardown() noexcept {
                int ret = 0;
                if (client_) {
                    ret = jack_client_close(client_);
                    client_ = nullptr;
                }
                inPorts_.clear();
                outPorts_.clear();
                inBuffers_.clear();
                outBuffers_.clear();
                inSources_.clear();
                outSinks_.clear();
                return ret;
            }

            jack_client_t *client_ = nullptr;

            // Nos ports.
            std::vector<jack_port_t *> inPorts_;
            std::vector<jack_port_t *> outPorts_;

            // Tableaux de pointeurs passés au callback utilisateur. Dimensionnés
            // dans open_ : le thread audio ne fait aucune allocation.
            std::vector<const float *> inBuffers_;
            std::vector<float *> outBuffers_;

            // Ports de l'endpoint à connecter à chaque start_ (vides si id vide).
            std::vector<std::string> inSources_;
            std::vector<std::string> outSinks_;
    };

    // --- Thread audio -------------------------------------------------------

    int JACK::onProcess(const jack_nframes_t nframes, void *arg) {
        auto *self = static_cast<JACK *>(arg);

        for (std::size_t i = 0; i < self->inPorts_.size(); ++i) {
            self->inBuffers_[i] = static_cast<const float *>(jack_port_get_buffer(self->inPorts_[i], nframes));
        }
        for (std::size_t i = 0; i < self->outPorts_.size(); ++i) {
            self->outBuffers_[i] = static_cast<float *>(jack_port_get_buffer(self->outPorts_[i], nframes));
        }

        if (self->callback) {
            const AudioProcessContext ctx{
                .input = { self->inBuffers_.data(), static_cast<std::uint32_t>(self->inBuffers_.size()) },
                .output = { self->outBuffers_.data(), static_cast<std::uint32_t>(self->outBuffers_.size()) },
                .frames = nframes
            };
            self->callback(ctx);
        } else {
            // Les buffers de sortie JACK ne sont pas garantis nuls.
            for (float *out : self->outBuffers_) {
                std::memset(out, 0, nframes * sizeof(float));
            }
        }
        return 0;
    }

    // --- open_ / start_ / stop_ / close_ -----------------------------------

    Result JACK::open_(EndpointConfig const &endpointCfg) {
        // Vérifications locales, sans toucher au serveur.
        const auto contains = [](auto const &arr, auto value) {
            return std::find(arr.begin(), arr.end(), value) != arr.end();
        };
        if (!contains(supportedSampleRates, endpointCfg.sampleRate))
            return std::unexpected{ ErrorType::SampleRateNotSupported };
        if (!contains(supportedBufferSizes, endpointCfg.bufferSize))
            return std::unexpected{ ErrorType::BufferSizeNotSupported };
        if (endpointCfg.format != Format::Float32) // les ports JACK sont toujours float32
            return std::unexpected{ ErrorType::FormatNotSupported };

        const bool wantIn = endpointCfg.direction != Direction::Output;
        const bool wantOut = endpointCfg.direction != Direction::Input;
        const std::uint32_t nIn = wantIn ? endpointCfg.inputChannels : 0;
        const std::uint32_t nOut = wantOut ? endpointCfg.outputChannels : 0;
        if ((wantIn && nIn == 0) || (wantOut && nOut == 0))
            return std::unexpected{ ErrorType::ChannelsNotSupported };

        // Connexion au serveur (jamais lancé automatiquement).
        jack_status_t status{};
        client_ = jack_client_open("mka-audio", JackNoStartServer, &status);
        if (!client_) return std::unexpected{ ErrorType::EndpointUnavailable };

        const auto fail = [this](const ErrorType error) -> Result {
            teardown();
            return std::unexpected{ error };
        };

        // Endpoint demandé : doit exister avec la capacité voulue et assez de ports.
        if (!endpointCfg.id.empty()) {
            if (wantIn) {
                inSources_ = portsOf(client_, endpointCfg.id, JackPortIsOutput);
                if (inSources_.empty()) return fail(ErrorType::EndpointUnavailable);
                if (inSources_.size() < nIn) return fail(ErrorType::ChannelsNotSupported);
                inSources_.resize(nIn);
            }
            if (wantOut) {
                outSinks_ = portsOf(client_, endpointCfg.id, JackPortIsInput);
                if (outSinks_.empty()) return fail(ErrorType::EndpointUnavailable);
                if (outSinks_.size() < nOut) return fail(ErrorType::ChannelsNotSupported);
                outSinks_.resize(nOut);
            }
        }

        // Rate et buffer sont imposés par le serveur : on valide, on ne négocie pas.
        if (jack_get_sample_rate(client_) != endpointCfg.sampleRate)
            return fail(ErrorType::SampleRateNotSupported);
        if (jack_get_buffer_size(client_) != endpointCfg.bufferSize)
            return fail(ErrorType::BufferSizeNotSupported);

        // Nos ports : ceux qui reçoivent sont JackPortIsInput, et inversement.
        for (std::uint32_t i = 0; i < nIn; ++i) {
            const std::string name = "in_" + std::to_string(i + 1);
            jack_port_t *port = jack_port_register(client_, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            if (!port) return fail(ErrorType::ConfigurationFailed);
            inPorts_.push_back(port);
        }
        for (std::uint32_t i = 0; i < nOut; ++i) {
            const std::string name = "out_" + std::to_string(i + 1);
            jack_port_t *port = jack_port_register(client_, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            if (!port) return fail(ErrorType::ConfigurationFailed);
            outPorts_.push_back(port);
        }
        inBuffers_.resize(nIn);
        outBuffers_.resize(nOut);

        if (jack_set_process_callback(client_, &JACK::onProcess, this) != 0)
            return fail(ErrorType::ConfigurationFailed);

        return {};
    }

    Result JACK::start_() {
        // jack_activate crée/démarre le thread audio du client.
        if (jack_activate(client_) != 0)
            return std::unexpected{ ErrorType::ConfigurationFailed };

        // jack_deactivate (stop_) retire les connexions : on les refait ici.
        // EEXIST = déjà connecté, pas une erreur.
        const auto connect = [this](const char *src, const char *dst) {
            const int ret = jack_connect(client_, src, dst);
            return ret == 0 || ret == EEXIST;
        };

        bool ok = true;
        for (std::size_t i = 0; ok && i < inSources_.size(); ++i) {
            ok = connect(inSources_[i].c_str(), jack_port_name(inPorts_[i]));
        }
        for (std::size_t i = 0; ok && i < outSinks_.size(); ++i) {
            ok = connect(jack_port_name(outPorts_[i]), outSinks_[i].c_str());
        }

        if (!ok) {
            jack_deactivate(client_);
            return std::unexpected{ ErrorType::EndpointUnavailable };
        }
        return {};
    }

    Result JACK::stop_() {
        // Retire le client du graphe : le serveur arrête d'appeler le callback
        // et jack_deactivate ne rend la main qu'une fois le thread audio quitté.
        if (jack_deactivate(client_) != 0)
            return std::unexpected{ ErrorType::ConfigurationFailed };
        return {};
    }

    Result JACK::close_() {
        if (teardown() != 0)
            return std::unexpected{ ErrorType::ConfigurationFailed };
        return {};
    }

    // --- getEndPoints_ ------------------------------------------------------

    std::vector<Endpoint> JACK::getEndPoints_() const {
        try {
            jack_status_t status{};
            const ClientPtr client{ jack_client_open("mka-audio-scan", JackNoStartServer, &status), &jack_client_close };
            if (!client) return {};

            const PortList ports{ jack_get_ports(client.get(), nullptr, JACK_DEFAULT_AUDIO_TYPE, 0), &jack_free };
            if (!ports) return {};

            const SampleRate rate = jack_get_sample_rate(client.get());
            const BufferSize bufferSize = jack_get_buffer_size(client.get());

            std::vector<Endpoint> endpoints;
            for (const char **p = ports.get(); *p; ++p) {
                const jack_port_t *port = jack_port_by_name(client.get(), *p);
                if (!port) continue;

                const std::string_view fullName{*p};
                const auto colon = fullName.find(':');
                if (colon == std::string_view::npos) continue;
                const std::string clientName{fullName.substr(0, colon)};

                auto it = std::ranges::find_if(endpoints, [&](const Endpoint &e) { return e.id == clientName; });
                if (it == endpoints.end()) {
                    endpoints.push_back(Endpoint{ .id = clientName, .name = clientName });
                    it = std::prev(endpoints.end());
                }

                // Port de sortie JACK = source pour nous (capacité input), et inversement.
                auto &capability = (jack_port_flags(port) & JackPortIsOutput) ? it->input : it->output;
                if (!capability) {
                    capability = StreamCapabilities{
                        .minChannels = 1,
                        .maxChannels = 0,
                        .sampleRates = { rate },
                        .formats = { Format::Float32 },
                        .bufferSizes = { bufferSize },
                    };
                }
                ++capability->maxChannels;
            }
            return endpoints;
        } catch (...) {
            return {};
        }
    }

}
