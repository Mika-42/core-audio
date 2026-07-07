#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.jack;

void audio_callback(mka::audio::Block& block) {
    static float phase = 0.0f;

    const float sampleRate = static_cast<float>(block.sampleRate);
    const float frequency  = 440.0f;
    const float amplitude  = 1.0f; // évite tout risque de clipping

    const float twoPi = 2.0f * std::numbers::pi_v<float>;
    const float phaseIncrement = twoPi * frequency / sampleRate;

    for (uint32_t i = 0; i < block.blockSize; ++i) {
      // Génération du sample
      float sample = amplitude * std::sinf(phase);

      // Avance de phase
      phase += phaseIncrement;
        // Wrap propre (évite dérive)
      if (phase >= twoPi)
          phase -= twoPi;

      // Écriture dans toutes les sorties
      for (uint32_t ch = 0; ch < block.outputCount; ++ch) {
        block.outputs[ch][i] = sample;
      }
    }
}

int main() {
	mka::audio::JACK engine;
	
	// [0] list all availables channels
	auto channels = engine.getChannels();
    
  for (auto& ch : channels) {
		  std::println("channel name:\t{}", ch.name);
  }
	
	// [1] select one of them
	int choice = 0;
	std::print(">> enter number: ");
	std::cin >> choice;
	
	// [2] setup the engine
	engine.setCallback(audio_callback);
	engine.setSampleRate(48'000);
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

