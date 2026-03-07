#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.jack;

float midiToFreq(int midiNote) {
    return 440.0f * std::pow(2.0f, (midiNote - 69) / 12.0f);
}
/*
void audio_callback(mka::audio::Block& block) {

	static float phase[128] = {};
	constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;

	for(uint32_t i = 0; i < block.blockSize; ++i) {
		float sample = 0;
		for (uint32_t idx = 0; idx < block.midiEventCount; ++idx) {
			const auto& midi = block.midiEvents[idx];

			if(midi.frameOffset != 0) {
				continue;
			}

			sample += midi.data[2] / 127.0f * sinf(phase[idx]);

			phase[idx] += twoPi * midiToFreq(midi.data[1]) / static_cast<float>(block.sampleRate);
			if (phase[idx] >= twoPi) {
				phase[idx] -= twoPi;
			}
		}

		sample /= static_cast<float>(block.midiEventCount);

		for(uint32_t ch = 0; ch < block.outputCount; ++ch) {
			block.outputs[ch][i] = sample;
		}
	}
}
*/

void audio_callback(mka::audio::Block& block)
{
    static float phase[128] = {};
    static bool noteActive[128] = {};
    static float velocity[128] = {};

    constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;

    uint32_t evtIndex = 0;

    for(uint32_t frame = 0; frame < block.blockSize; ++frame)
    {
        // appliquer les events MIDI au bon sample
        while(evtIndex < block.midiEventCount &&
              block.midiEvents[evtIndex].frameOffset == frame)
        {
            const auto& midi = block.midiEvents[evtIndex];

            uint8_t status = midi.data[0] & 0xF0;
            uint8_t note   = midi.data[1];
            uint8_t vel    = midi.data[2];

            if(status == 0x90 && vel > 0) // note on
            {
                noteActive[note] = true;
                velocity[note] = vel / 127.0f;
            }
            else if(status == 0x80 || (status == 0x90 && vel == 0)) // note off
            {
                noteActive[note] = false;
            }

            evtIndex++;
        }

        float sample = 0.0f;
        int activeCount = 0;

        // synthèse
        for(int note = 0; note < 128; ++note)
        {
            if(!noteActive[note])
                continue;

            float freq = midiToFreq(note);

            sample += velocity[note] * sinf(phase[note]);

            phase[note] += twoPi * freq / block.sampleRate;
            if(phase[note] >= twoPi)
                phase[note] -= twoPi;

            activeCount++;
        }

        if(activeCount > 0)
            sample /= activeCount;

        for(uint32_t ch = 0; ch < block.outputCount; ++ch)
        {
            block.outputs[ch][frame] = sample;
        }
    }
}
int main() {
	mka::audio::JACK engine;
	
	// [0] list all availables channels
	auto channels = engine.getChannels();
    for (auto& ch : channels) {	
		std::println("channel name:\t{}", ch.name);
		std::println("direction:\t{}\n", ch.direction == mka::audio::Direction::In ? "Input" : "Output");
    }
	
	// [1] select one of them
	int choice = 0;
	std::print(">> enter number: ");
	std::cin >> choice;

	//[]	
	auto MIDIchannels = engine.getMidiDevices();
    for (auto& ch : MIDIchannels) {	
		std::println("channel name:\t{}", ch.name);
		std::println("direction:\t{}\n", ch.direction == mka::audio::Direction::In ? "Input" : "Output");
    }

	std::print(">> enter number: ");
	std::cin >> choice;
	
	engine.mapMidiDevice(MIDIchannels[choice], mka::audio::MIDI::Channel1);

	// [2] setup the engine
	engine.setCallback(audio_callback);
	engine.setSampleRate(22'050);
	engine.setBlockSize(1024);

	// [3] open the desire channel
	auto ret = engine.open(channels[choice]);
	if(!ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	// [4] start the engine
	engine.start();
	
	std::println("press any key to stop audio...");
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	std::cin.get();

	// [5] stop the engine
	engine.stop();
	
	// [] close the engine
	ret = engine.close();
	if(!ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	return 0;
}

