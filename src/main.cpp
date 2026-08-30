#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.engine;
import audio.block;
import audio.alsa;

int main() {

	mka::audio::ALSA backend;
	auto ret = backend.getDevices();
	
	for (const auto& device : ret)
    {
        std::cout << "========================================\n";
        std::cout << "ID:             " << device.id << '\n';
        std::cout << "Name:           " << device.name << '\n';

        std::cout << "Input channels: " << device.inputChannels << '\n';
        std::cout << "Output channels:" << device.outputChannels << '\n';

        std::cout << "Sample rates:   ";

        for (size_t i = 0; i < device.sampleRates.size(); ++i)
        {
            if (i != 0)
                std::cout << ", ";

            std::cout << device.sampleRates[i];
        }

        std::cout << '\n';

        std::cout << "Buffer sizes:   ";

        for (size_t i = 0; i < device.bufferSizes.size(); ++i)
        {
            if (i != 0)
                std::cout << ", ";

            std::cout << device.bufferSizes[i];
        }

        std::cout << '\n';

        std::cout << "Sample formats: ";

        for (size_t i = 0; i < device.sampleFormats.size(); ++i)
        {
            if (i != 0)
                std::cout << ", ";

            switch (device.sampleFormats[i])
            {
                case mka::audio::SampleFormat::Int16:
                    std::cout << "Int16";
                    break;

                case mka::audio::SampleFormat::Int24:
                    std::cout << "Int24";
                    break;

                case mka::audio::SampleFormat::Int32:
                    std::cout << "Int32";
                    break;

                case mka::audio::SampleFormat::Float32:
                    std::cout << "Float32";
                    break;

                case mka::audio::SampleFormat::Float64:
                    std::cout << "Float64";
                    break;

                default:
                    std::cout << "Invalid";
                    break;
            }
        }

        std::cout << "\n\n";
    }

	return 0;
}

