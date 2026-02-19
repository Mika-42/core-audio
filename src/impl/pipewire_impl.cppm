module;

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

export module audio.pipewire;

export import audio.block;
export import audio.config;
export import audio.error;

import audio.abstract_core;

namespace {

struct NodeInfo {
	uint32_t id = 0;
	uint32_t deviceId = 0;
};

struct DiscoveryState {
	std::unordered_map<uint32_t, mka::audio::Device> devices;
	std::unordered_map<uint32_t, NodeInfo> nodes;
	int seq = -1;
};

static bool hasChannel(const mka::audio::Device& device, const std::string& channelName, bool input) {
	for (const auto& channel : device.channels) {
		if (channel.name == channelName && channel.input == input) {
			return true;
		}
	}
	return false;
}

static void fillDefaultCapabilities(mka::audio::Device& device) {
	device.formats = {
		mka::audio::Format::Int16,
		mka::audio::Format::Int24,
		mka::audio::Format::Int32,
		mka::audio::Format::Float32,
		mka::audio::Format::Float64,
	};

	device.samplerates.assign(std::begin(mka::audio::supported::Samplerates), std::end(mka::audio::supported::Samplerates));
	device.bufferSizes.assign(std::begin(mka::audio::supported::bufferSizes), std::end(mka::audio::supported::bufferSizes));
}

static mka::audio::Device& ensureDevice(DiscoveryState& state, uint32_t deviceId, const char* fallbackName = nullptr) {
	auto& device = state.devices[deviceId];
	device.id = deviceId;

	if (device.name.empty() && fallbackName && fallbackName[0] != '\0') {
		device.name = fallbackName;
	}

	if (device.formats.empty() || device.samplerates.empty() || device.bufferSizes.empty()) {
		fillDefaultCapabilities(device);
	}

	return device;
}

static void registryEventGlobal(
		void* data,
		uint32_t id,
		[[maybe_unused]] uint32_t permissions,
		const char* type,
		[[maybe_unused]] uint32_t version,
		const struct spa_dict* props) {

	if (!data || !type || !props) {
		return;
	}

	auto* state = static_cast<DiscoveryState*>(data);

	if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
		const char* description = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION);
		const char* nickname = spa_dict_lookup(props, PW_KEY_DEVICE_NICK);
		const char* name = spa_dict_lookup(props, PW_KEY_DEVICE_NAME);

		const char* chosenName = description ? description : (nickname ? nickname : name);
		ensureDevice(*state, id, chosenName);
		return;
	}

	if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char* deviceIdText = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
		if (!deviceIdText) {
			return;
		}

		const uint32_t deviceId = static_cast<uint32_t>(std::strtoul(deviceIdText, nullptr, 10));
		state->nodes[id] = NodeInfo{ .id = id, .deviceId = deviceId };
		ensureDevice(*state, deviceId);
		return;
	}

	if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		const char* nodeIdText = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char* directionText = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
		if (!nodeIdText || !directionText) {
			return;
		}

		const uint32_t nodeId = static_cast<uint32_t>(std::strtoul(nodeIdText, nullptr, 10));
		auto nodeIt = state->nodes.find(nodeId);
		if (nodeIt == state->nodes.end()) {
			return;
		}

		const char* prettyName = spa_dict_lookup(props, PW_KEY_PORT_ALIAS);
		const char* displayName = spa_dict_lookup(props, PW_KEY_PORT_NAME);
		const char* fallbackName = spa_dict_lookup(props, PW_KEY_OBJECT_PATH);
		const char* channelNameText = prettyName ? prettyName : (displayName ? displayName : fallbackName);
		if (!channelNameText) {
			return;
		}

		const bool isInput = std::strcmp(directionText, "in") == 0;
		auto& device = ensureDevice(*state, nodeIt->second.deviceId);

		if (!hasChannel(device, channelNameText, isInput)) {
			device.channels.push_back(mka::audio::Channel{ .name = channelNameText, .input = isInput });
		}
	}
}

static const pw_registry_events registryEvents{
	.version = PW_VERSION_REGISTRY_EVENTS,
	.global = registryEventGlobal,
};

struct SyncData {
	DiscoveryState* state = nullptr;
	pw_main_loop* loop = nullptr;
};

static void coreEventDoneWithLoop(void* data, uint32_t, int seq) {
	auto* syncData = static_cast<SyncData*>(data);
	if (!syncData || !syncData->state || !syncData->loop) {
		return;
	}

	if (seq == syncData->state->seq) {
		pw_main_loop_quit(syncData->loop);
	}
}

static const pw_core_events coreEvents{
	.version = PW_VERSION_CORE_EVENTS,
	.done = coreEventDoneWithLoop,
};

} // namespace

export namespace mka::audio {

class PipeWire : public AbstractCoreAudio {
public:
	PipeWire() {
		// IMPORTANT:
		// On initialise PipeWire au niveau de l'instance backend, pas dans une fonction utilitaire.
		// Cela évite les init/deinit répétés et prépare une architecture propre pour le futur streaming temps réel.
		pw_init(nullptr, nullptr);
		initialized = initRuntime();
	}

	~PipeWire() override {
		shutdownRuntime();
		pw_deinit();
	}

	std::vector<Device> devicesList() override {
		if (!initialized) {
			return {};
		}

		// On sérialise l'énumération pour ne pas exécuter plusieurs discovery en parallèle
		// sur la même boucle PipeWire. C'est plus sûr et plus simple à maintenir.
		std::lock_guard<std::mutex> lock(discoveryMutex);
		return discoverDevicesOnce();
	}

	Result open(const Config& cfg) override {
		config = cfg;
		return Result{ Error::DeviceOpenFailed, "PipeWire streaming backend is not implemented yet" };
	}

	Result close() override {
		return Ok;
	}

protected:
	void run() override {}

private:
	bool initRuntime() {
		loop = pw_main_loop_new(nullptr);
		if (!loop) {
			return false;
		}

		context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
		if (!context) {
			shutdownRuntime();
			return false;
		}

		core = pw_context_connect(context, nullptr, 0);
		if (!core) {
			shutdownRuntime();
			return false;
		}

		return true;
	}

	void shutdownRuntime() {
		if (core) {
			pw_core_disconnect(core);
			core = nullptr;
		}

		if (context) {
			pw_context_destroy(context);
			context = nullptr;
		}

		if (loop) {
			pw_main_loop_destroy(loop);
			loop = nullptr;
		}
	}

	std::vector<Device> discoverDevicesOnce() {
		DiscoveryState state;
		std::vector<Device> discovered;

		if (!core || !loop) {
			return discovered;
		}

		// Le registry est une vue dynamique de l'univers PipeWire courant.
		// On l'ouvre pour la découverte, puis on le détruit immédiatement après.
		pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
		if (!registry) {
			return discovered;
		}

		spa_hook registryListener{};
		spa_hook coreListener{};
		SyncData syncData{ .state = &state, .loop = loop };

		pw_registry_add_listener(registry, &registryListener, &registryEvents, &state);
		pw_core_add_listener(core, &coreListener, &coreEvents, &syncData);

		// On envoie un sync token puis on bloque la loop jusqu'à l'ack.
		// Cela garantit une liste complète (Device/Node/Port) à l'instant T.
		state.seq = pw_core_sync(core, PW_ID_CORE, 0);
		pw_main_loop_run(loop);

		for (auto& [_, device] : state.devices) {
			discovered.push_back(std::move(device));
		}

		std::sort(discovered.begin(), discovered.end(), [](const Device& left, const Device& right) {
			return left.name < right.name;
		});

		spa_hook_remove(&coreListener);
		spa_hook_remove(&registryListener);
		pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));

		return discovered;
	}

	pw_main_loop* loop = nullptr;
	pw_context* context = nullptr;
	pw_core* core = nullptr;
	bool initialized = false;
	std::mutex discoveryMutex;
};

} // namespace mka::audio
