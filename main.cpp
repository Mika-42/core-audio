#include "alsa/alsa-core-audio.hpp"
#include <thread>
#include <print>
#include <iostream>

int main() 
{
	ALSA alsa;
	Device I, O;
	I.id = O.id = "default";
	I.type = Input;
	O.type = Output;

	auto err = alsa.open(I, O)
	.and_then([&]{ return alsa.setChannels(2); })
	.and_then([&]{ return alsa.setFrames(512); })
	.and_then([&]{ return alsa.setSamplerate(48000); });
	
	if(!err) std::println("{}", err.error());

	std::jthread audioThread([&]{alsa.audioLoop();});

// attendre que l'utilisateur appuie sur Entrée pour stopper
	std::println("Press Enter to stop audio...\n");
	std::cin.get();

	alsa.close();
	return 0;
}
