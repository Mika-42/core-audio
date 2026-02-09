## Minimal code exemple
C++ version : 26

### 1. import the audio core
```cpp
import audio.<type>;
```
<type> = alsa, jack, wasapi, asio, coreaudio, etc...
replace with the desire plateform.

### 2. setup the callback function

**2.1 - function signature**

```cpp
void <callback_name>(const mka::audio::Block& block);
```

The callback function **must obligatory** respect this signature. 

**2.2 - mka::audio::Block structure**

```cpp
block.samplerate;                // read only uint32_t value
block.channels;                  // read only uint32_t value
block.frames;                    // read only uint32_t value
block.out[<channel>][<frame>];   // read and write float value
block.in[<channel>][<frame>];    // read and write float value
```

Thanks to this structure you can operate over the buffer and access to related datas like samplerate, channels and frames.

### 3. configuration

```cpp
	config.samplerate;
	config.buffersize;
	config.channels;
	config.name;
```

This struct describe how you audio channel should be.

### 4. pipeline

your code must following this pipeline :

```
(open)->(set callback)->(start)->(stop)->(close)
```

### 5. Full code

ALSA exemple : 

```cpp
#include <print>
#include <iostream>
#include <cmath>

import audio.alsa;

void audio_callback(const mka::audio::Block& block) {

	for(uint32_t channel = 0; channel < block.channels; ++channel) {
	    for(uint32_t frame = 0; frame < block.frames; ++frame) {	
			block.out[channel][frame] = /* feed */; 
		}
	}
}

int main() 
{

	mka::audio::ALSA alsa;
	
	mka::audio::config config {
		.samplerate = 44100,
		.buffersize = 512,
		.channels = 2,
		.name = "default"
	};

	auto ret = alsa.open(config);
	if(!reti.ok()) std::println("error::{}", ret.message);

	alsa.setCallback(audio_callback);
	alsa.start();
	
	std::println("Press Enter to stop audio...\n");
	std::cin.get();
	
	alsa.stop();

	ret = alsa.close();
	if(!ret.ok()) std::println("error::{}", ret.message);

	return 0;
}
```
