module;
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
export module audio.engine;
export import audio.block;
import audio.routing_table;

namespace mka::audio {
	
	struct BackendBuffer {
		float* const* data;
		size_t channels;
		size_t frames;
	};
	
	[[nodiscard]] bool is_full(const Block& b) noexcept {
		return b.validFrames >= b.capacityFrames();
	}
	
	size_t resolve_and_accumulate(BackendBuffer& input, const RoutingTable& routing, size_t offset, Block& workingBlock) {

		// frames restantes dans le block de travail
		const size_t remaining = workingBlock.capacityFrames()  - workingBlock.validFrames;

		// frames restante à copier dans le buffer d'entrée par block de taille `remaining` maximum
		const size_t chunk = std::min(input.frames - offset, remaining);

		// s'il n'y a plus de frames à copier, quitter la fonction
		if(chunk == 0) {
			return 0;
		}
		
		const size_t cursor = workingBlock.validFrames;

		for(size_t ch=0; ch<routing.channels(); ++ch) {

			const auto backendIdx = routing.input(ch).get();
		
			if (workingBlock.channel(ch) == nullptr) {
				continue;
			}

			float *dest = workingBlock.channel(ch) + cursor;

			if(!backendIdx) {
				std::memset(dest, 0, chunk * sizeof(float));
				continue;
			}

			if(*backendIdx >= input.channels) {
				std::memset(dest, 0, chunk * sizeof(float));
				continue;
			}

			if(input.data[*backendIdx] == nullptr) {
				std::memset(dest, 0, chunk * sizeof(float));
				continue;
			}

			const float *source = input.data[*backendIdx] + offset;
			std::memcpy(dest, source, chunk * sizeof(float));
		}

		workingBlock.validFrames = cursor + chunk;

		return chunk;
	}

	// complexite : O(output.channels + routing.channels() * route.size() * framesToCopy)
	size_t copy_pending_and_resolve(BackendBuffer& output, const RoutingTable& routing, size_t offset, Block& workingBlock) {
		
		// si toutes les frames ont déjà été copier, nettoyer, quitter
		if(workingBlock.cursor >= workingBlock.validFrames) {	
			workingBlock.clear();	
			return 0;	
		}	
		
		// frames disponible pour la copie dans le bloc
		const size_t available = workingBlock.validFrames - workingBlock.cursor;
		
		// frames vide restantes dans le buffer de sortie
		const size_t remaining = output.frames - offset;

		const size_t framesToCopy = std::min(available, remaining);
	
		for(size_t ch=0; ch<output.channels; ++ch) {
            if (output.data[ch] == nullptr) {
                continue;
            }

            float* dest = output.data[ch] + offset;

            std::memset(dest, 0, framesToCopy * sizeof(float));
        }

		//TODO optimize 3 for-loop imbrication
		for(size_t ch=0; ch<routing.channels(); ++ch) {

			const auto route = routing.output(ch).get();
			
			const float *source = workingBlock.channel(ch) + workingBlock.cursor;
			
			for (size_t i=0; i<route.size(); ++i) {
				
				if(output.data[route[i]] == nullptr) {
					continue;
				}

				float *dest = output.data[route[i]] + offset;
	
				// mixer toutes les sources
				for(size_t frame=0; frame<framesToCopy; ++frame) {
					dest[frame] += source[frame];
				}
			}
		}
		workingBlock.cursor += framesToCopy;
		return framesToCopy;
	}

	export using ProcessBlockFn = void (*)(const BlockView&, BlockView&);

	void audio_process(BackendBuffer& input, BackendBuffer& output, 
			const RoutingTable& routing,
			Block& workingBlockInput,
			Block& workingBlockOutput, ProcessBlockFn processBlock) {

		size_t inputOffset = 0;
		size_t outputOffset = 0;

		// todo if out pending frames, copy 	workingBlockOutput content to output ptr and move cursor
		outputOffset += copy_pending_and_resolve(output, routing, outputOffset, workingBlockOutput);

		// si le buffer est plein et qu'il reste des frames en attentes 
		// alors quitter la fonction et attendre le prochain appel pour les copier
		if(outputOffset >= output.frames) {
			return;
		} 

		while(inputOffset < input.frames && outputOffset < output.frames) {
			
			const size_t accumulated = resolve_and_accumulate(
					input, routing, inputOffset, workingBlockInput
			);

			// s'il est impossible d'incrémenter `offset` alors quitter la boucle
			if(accumulated == 0) {
				break;
			}
			
			inputOffset += accumulated;
	
			// attendre que le bloc soit plein avant de le livrer
			if(!is_full(workingBlockInput)) {
				continue;
			}

			BlockView input_view(workingBlockInput);
			BlockView output_view(workingBlockOutput);
			// traiter les blocks
			if(processBlock) {
				processBlock(input_view, output_view);
			}

			workingBlockOutput.validFrames = workingBlockOutput.capacityFrames();
			workingBlockOutput.cursor = 0;
	
			outputOffset += copy_pending_and_resolve(output, routing, outputOffset, workingBlockOutput);
		}		
	}

	struct EngineConfig {
		size_t channels = 0;
		size_t blockSize = 0;
	};

	// contrat : routing.channels == workIn_.channels == workOut.channels
	class Engine {
		public:
	
		Engine(const EngineConfig& config) : routing_(config.channels) {
			workIn_.configure(config.channels, config.blockSize);
			workOut_.configure(config.channels, config.blockSize);
		}

		void process(BackendBuffer& input, BackendBuffer& output, ProcessBlockFn processBlock) {
			audio_process(input, output, routing, workIn_, workOut_, processBlock);
		}


		RoutingTable routing;
		private:
			Block workIn_ {};
			Block workOut_ {};

	};
}
