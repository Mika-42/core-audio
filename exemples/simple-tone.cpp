#include "alsa/alsa-core-audio.hpp"
#include <thread>
#include <print>
#include <iostream>
#include <cmath>

void processAudio(
	const float** in, float** out, 
	const unsigned long long int channels, 
	const unsigned long long int frames, 
	const unsigned long long int samplerate)
{
	static unsigned long long int i = 0;

	for(unsigned int frame = 0; frame < frames; ++frame)
	{	
        	for (unsigned int channel = 0; channel < channels; ++channel)
        	{
                	static constexpr float Tau = 2.0f * 3.1415926535f;
			static constexpr float Freq = 440.0f;
			
			const float Phase = Tau / samplerate;
			out[channel][frame] = std::sin(Phase * Freq * i);
                }
		++i;
        }
}

int main() 
{
	ALSA audioCore;
	Device I 
	{
		.id = "default",
		.type = Input,
	};

	Device O
	{
		.id = "default",
		.type = Output,
	};

	auto err = audioCore.open(I, O)
	.and_then([&]{ return audioCore.setChannels(2); })
	.and_then([&]{ return audioCore.setFrames(512); })
	.and_then([&]{ return audioCore.setSamplerate(44100); });
	
	if(!err) std::println("{}", err.error());

	std::jthread audioThread([&]{audioCore.audioLoop();});

	// attendre que l'utilisateur appuie sur Entrée pour stopper
	std::println("Press Enter to stop audio...\n");
	std::cin.get();

	audioCore.close();
	return 0;
}
