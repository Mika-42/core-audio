module;

#include <memory>
#include <string>
#include <thread>
#include <atomic>

export module audio.jack;
export import audio.block;
export import audio.config;

import audio.error;
import audio.abstract_core;

export namespace mka::audio {
	
	class JACK final: public AbstractCoreAudio {

	public:

		JACK() = default;

		Result open(const Config& config) override {}
		Result close() override {}
}


