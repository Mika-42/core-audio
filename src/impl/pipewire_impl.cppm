module;
#include <print>
#include <thread>

#include <vector>
#include <string>
#include <unordered_map>
#include <pipewire/pipewire.h>
export module audio.pipewire;
export import audio.block;
export import audio.config;
export import audio.error;

import audio.abstract_core;
/*	struct Device {

		std::string				name;

		uint32_t				minInputs;
		uint32_t				minOutputs;

		uint32_t				maxInputs;
		uint32_t				maxOutputs;

		std::vector<Format>		formats;
		std::vector<uint32_t>	samplerates;
		std::vector<uint32_t>	bufferSizes;

	};
*/
struct NodeInfo {
    uint32_t deviceId = 0;
    // plus tard tu pourras ajouter :
    // std::string mediaClass;
};

static std::unordered_map<uint32_t, mka::audio::Device> devices;
static std::unordered_map<uint32_t, NodeInfo> nodes;

static void registry_event_global(
		void *data, 
		uint32_t id, 
		uint32_t permissions, 
		const char *type, 
		uint32_t version, 
		const struct spa_dict *props) {
	
	if (!props) return;

	// Devices	
	if (strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
		const char* description = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION); 
		
		if (!description) return;
		
		auto& device	= devices[id];
		device.name		= description;
		device.id		= id;

//		std::println("({}) {}", device.id, device.name); 
	
	}

	// Nodes
	else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
	    const char* device_id = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
		
		if (!device_id) return;

		const uint32_t dev_id = std::stoi(device_id);

        NodeInfo info;
        info.deviceId = dev_id;

        nodes[id] = info;
	}

	// Ports
	else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {	
	    const char* node_id		= spa_dict_lookup(props, PW_KEY_NODE_ID);
	    const char* direction	= spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
	    const char* port_name	= spa_dict_lookup(props, PW_KEY_PORT_NAME);
		
		if (!node_id || !direction || !port_name) return;

		const uint32_t nde_id = std::stoi(node_id);

		auto nodeIt = nodes.find(nde_id);
        if (nodeIt == nodes.end()) return;

        uint32_t deviceId = nodeIt->second.deviceId;

        auto devIt = devices.find(deviceId);
        if (devIt == devices.end()) return;

        mka::audio::Channel channel;
        channel.id	  = nde_id;
		channel.name  = port_name;
        channel.input = (strcmp(direction, "in") == 0);

        devIt->second.channels.push_back(std::move(channel));
	}

}
 
static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
};

export namespace mka::audio {

	class PipeWire : public AbstractCoreAudio
	{

	public:
		PipeWire() {
			pw_init(nullptr, nullptr);

			loop		= pw_main_loop_new(nullptr);
			context		= pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
			core		= pw_context_connect(context, nullptr, 0);
			registry	= pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
		
			spa_zero(registry_listener);
			pw_registry_add_listener(registry, &registry_listener, &registry_events, nullptr);
		auto loopThread = std::thread([this]() {
			pw_main_loop_run(loop);
		});

		// Petite pause pour laisser le temps à la registry
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		pw_main_loop_quit(loop);
		loopThread.join();	
		}

		void show() {	
			for(const auto& [_, d] : devices) {
				std::println("Device {{"); 
				std::println("\tid: {},\n\tname: {},\n\tIO: {}", d.id, d.name, d.channels.size()); 

				for(const auto& c : d.channels) {
					std::println("\tChannel {{");
					std::println("\t\tid: {},", c.id); 
					std::println("\t\tname: {},", c.name); 
					std::println("\t\tinput: {}\n\t}}",c.input); 
				}
				std::println("}}\n");
			}
		}

		~PipeWire() {
			pw_proxy_destroy((struct pw_proxy*)registry);
	        pw_core_disconnect(core);
		    pw_context_destroy(context);
			pw_main_loop_destroy(loop);

		}

		std::vector<Device> deviceList() override {}
	
		virtual Result open(const Config& config) {}
		virtual	Result close() {}
virtual void run() {}
	private:
		struct pw_main_loop*	loop;
		struct pw_context*		context;
		struct pw_core*			core;
		struct pw_registry*		registry;
		struct spa_hook			registry_listener;
	};

}
