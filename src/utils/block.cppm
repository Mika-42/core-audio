module;

#include <cstdint>

export module audio.block;

export namespace mka::audio {
	
	struct Block {
		const uint32_t		samplerate;
		const uint32_t		channels;
		const uint32_t		frames;
		float* const* const	out;
		float* const* const	in;
	};

	typedef void(*Callback)(const Block&);
}
