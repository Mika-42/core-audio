#include "alsa/alsa-core-audio.hpp"

int main() 
{
	ALSA alsa;
	Device I, O;
	alsa.open(I, O);
	alsa.setChannels(2);
	alsa.setFrames(512);
	alsa.setSamplerate(192000);
	alsa.close();
	return 0;
}
