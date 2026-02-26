## Minimal code example
C++ version : 26

### 1. import the audio core

```cpp
import audio.<type>;
```

| type      | alsa  | jack  | pipewire | pulseaudio | wasapi  | asio    | ds      | wmme    | coreaudio |
|-----------|-------|-------|----------|------------|---------|---------|---------|---------|-----------|
| supported | no    | yes   | no       | no         | no      | no      | no      | no      | no        |
| plateform | linux | linux | linux    | linux      | windows | windows | windows | windows | macos     |


replace with the desire plateform.

### 2. configuration

**2.1 - setup sampleRate (optionnal)**

```cpp
engine.setSampleRate(<sampleRate>);
```

**2.2 - setup blockSize (optionnal)**

```cpp
engine.setBlockSize(<blockSize>);
```

**2.3 - setup the callback function**

function signature

```cpp
void <callback_name>(mka::audio::Block& block);
```
> [!WARNING]
> The callback function **must obligatory** respect this signature. 

**2.4 - mka::audio::Block structure**

```cpp
block.blockSize;                     // read only uint32_t value
block.sampleRate;                    // read only uint32_t value
block.inputCount;                    // read only uint32_t value
block.outputCount;                   // read only uint32_t value
block.outputs[<channel>][<frame>];   // read and write float value
block.inputs[<channel>][<frame>];    // read only float value
```

Thanks to this structure you can operate over the buffer and access to related datas like samplerate, channels and frames.

### 4. pipeline

your code must following this pipeline :

```mermaid
graph LR
	A(("Begin"))

	subgraph "Setup engine"
		B1(["set callback"])
		B2(["set sampleRate"])
		B3(["set blockSize"])
	end

	C(["open"])
	D(["start"])
	E(["stop"])
	F(["close"])
	G(("End"))

	B3 --> C --> D --> E --> F --> G
	A --> B1 --> B2 --> B3
	classDef step fill:#775500
  	class B1,B2,B3,C,D,E,F step;
	style A fill:#007700
	style G fill:#770000
```

### 5. Full code

simple_tone_generator example : 

```cpp
#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.jack;

void audio_callback(mka::audio::Block& block) {
	static float phase = {};
	constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
	const float phaseIncrement = twoPi * 440.0f / static_cast<float>(block.sampleRate);

	for(uint32_t i = 0; i < block.blockSize; ++i) {
		float sample = sinf(phase);

		phase += phaseIncrement;
		if (phase >= twoPi) {
			phase -= twoPi;
		}

		for(uint32_t ch = 0; ch < block.outputCount; ++ch) {
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
		std::println("direction:\t{}\n", ch.direction == mka::audio::Direction::In ? "Input" : "Output");
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
	
	// [6] close the engine
	ret = engine.close();
	if(!ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	return 0;
}
```
