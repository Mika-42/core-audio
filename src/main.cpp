#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <print>

import audio.pipewire;

void audio_callback(mka::audio::Block& block) {
	static float phase = 0.0f;
	constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
	const float phaseIncrement = twoPi * 440.0f / static_cast<float>(block.sampleRate);

	for (uint32_t i = 0; i < block.blockSize; ++i) {
		const float sample = std::sinf(phase) * 0.2f;

		phase += phaseIncrement;
		if (phase >= twoPi) {
			phase -= twoPi;
		}

		for (uint32_t ch = 0; ch < block.outputCount; ++ch) {
			block.outputs[ch][i] = sample;
		}
	}
}

int main() {
	mka::audio::PipeWire engine;

	auto channels = engine.getChannels();
	if (channels.empty()) {
		std::println("No ALSA channels available (default device).");
		return -1;
	}

	std::println("Available ALSA channels:");
	for (size_t i = 0; i < channels.size(); ++i) {
		const auto& ch = channels[i];
		std::println("[{}] {} ({})", i, ch.name, ch.direction == mka::audio::Direction::In ? "Input" : "Output");
	}

	int choice = -1;
	std::print(">> Select a channel index: ");
	std::cin >> choice;

	if (choice < 0 || static_cast<size_t>(choice) >= channels.size()) {
		std::println("Invalid channel index.");
		return -1;
	}

	engine.setCallback(audio_callback);
	engine.setSampleRate(48'000);
	engine.setBlockSize(256);

	auto ret = engine.open(channels[static_cast<size_t>(choice)]);
	if (!ret.ok()) {
		std::println("open failed: {}", ret.message ? ret.message : "unknown error");
		return -1;
	}

	engine.start();

	std::println("ALSA engine started. Press ENTER to stop...");
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	std::cin.get();

	engine.stop();
	ret = engine.close();
	if (!ret.ok()) {
		std::println("close failed: {}", ret.message ? ret.message : "unknown error");
		return -1;
	}

	return 0;
}
