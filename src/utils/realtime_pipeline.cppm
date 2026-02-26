module;

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <cstdint>

export module audio.realtime_pipeline;

export import audio.block;
export import audio.config;
import audio.constants;

export namespace mka::audio::realtime {

	// Ingest one backend input stream into the engine FIFO.
	// The backend delivers device-rate samples, we immediately resample to
	// engine-rate so the callback always sees a stable format.
	inline void ingestInput(Channel& channel,
						 const float* backendInput,
						 size_t backendFrames,
						 float* resampleScratch,
						 size_t scratchCapacity) {
		if (!backendInput || backendFrames == 0 || !resampleScratch || scratchCapacity == 0) {
			return;
		}

		const size_t copyCount = (backendFrames < constants::MAX_BLOCK_SIZE)
			? backendFrames
			: constants::MAX_BLOCK_SIZE;

		std::memcpy(channel.scratchBuffer, backendInput, sizeof(float) * copyCount);

		size_t consumed = 0;
		const size_t produced = channel.inputResampler.process(
			channel.scratchBuffer,
			copyCount,
			resampleScratch,
			scratchCapacity,
			consumed
		);
		(void)consumed;

		if (produced > 0) {
			channel.fifo.push(resampleScratch, produced);
		}
	}

	inline size_t estimateRequiredResamplerInputFrames(const ResamplerState& resampler,
									 size_t outputFrames) {
		// Keep iteration planning in sync with renderOutput(): budget only what the
		// resampler will really consume to avoid over-producing and FIFO oscillation.
		const double projected = resampler.phase
			+ (static_cast<double>(outputFrames) * resampler.step);
		size_t required = static_cast<size_t>(projected);

		// Until the streaming state is seeded, process() needs bootstrap context.
		if (!resampler.seeded) {
			required += 4;
		}

		return required;
	}

	inline size_t computeCallbackIterations(
		std::span<Channel*> inputChannels,
		std::span<Channel*> outputChannels,
		size_t backendFrames,
		size_t fixedBlockSize) {

		if (fixedBlockSize == 0) {
			return 0;
		}

		size_t iterations = 0;

		if (outputChannels.empty()) {
			iterations = inputChannels.empty() ? 1 : constants::MAX_ITERATION;
			for (Channel* input : inputChannels) {
				if (!input) continue;
				const size_t availableBlocks = input->fifo.available() / fixedBlockSize;
				iterations = std::min(iterations, availableBlocks);
			}
			return iterations;
		}

		size_t maxMissingEngineFrames = 0;
		for (Channel* output : outputChannels) {
			if (!output) continue;

			// Use the same phase-aware budgeting as renderOutput() to keep planning
			// deterministic and prevent jitter between produce/consume sides.
			const size_t required = estimateRequiredResamplerInputFrames(
				output->outputResampler,
				backendFrames
			);
			const size_t available = output->fifo.available();

			if (available < required) {
				maxMissingEngineFrames = std::max(maxMissingEngineFrames, required - available);
			}
		}

		if (maxMissingEngineFrames > 0) {
			iterations = (maxMissingEngineFrames + fixedBlockSize - 1) / fixedBlockSize;
		}

		if (!inputChannels.empty()) {
			size_t minInputAvailable = constants::MAX_FIFO_SIZE;
			for (Channel* input : inputChannels) {
				if (!input) continue;
				minInputAvailable = std::min(minInputAvailable, input->fifo.available());
			}

			const size_t maxFromInput = minInputAvailable / fixedBlockSize;
			iterations = std::min(iterations, maxFromInput);
		}

		return iterations;
	}

	inline void runEngine(
		Callback callback,
		uint32_t sampleRate,
		uint32_t blockSize,
		std::span<Channel*> inputChannels,
		std::span<Channel*> outputChannels,
		float* inputStorage,
		float* outputStorage,
		size_t iterations) {

		if (!callback || !inputStorage || !outputStorage || blockSize == 0) {
			return;
		}

		Block block {};
		block.blockSize = blockSize;
		block.sampleRate = sampleRate;
		block.inputCount = static_cast<uint32_t>(inputChannels.size());
		block.outputCount = static_cast<uint32_t>(outputChannels.size());

		for (size_t i = 0; i < inputChannels.size(); ++i) {
			block.inputs[i] = inputStorage + (i * constants::MAX_BLOCK_SIZE);
		}

		for (size_t i = 0; i < outputChannels.size(); ++i) {
			block.outputs[i] = outputStorage + (i * constants::MAX_BLOCK_SIZE);
		}

		for (size_t it = 0; it < iterations; ++it) {
			for (size_t i = 0; i < inputChannels.size(); ++i) {
				Channel* input = inputChannels[i];
				if (!input) continue;
				input->fifo.pop(block.inputs[i], blockSize);
			}

			for (size_t i = 0; i < outputChannels.size(); ++i) {
				std::memset(block.outputs[i], 0, sizeof(float) * blockSize);
			}

			callback(block);

			for (size_t i = 0; i < outputChannels.size(); ++i) {
				Channel* output = outputChannels[i];
				if (!output) continue;
				output->fifo.push(block.outputs[i], blockSize);
			}
		}
	}

	inline size_t renderOutput(Channel& channel,
						float* backendOutput,
						size_t backendFrames,
						float* resampleScratch,
						size_t scratchCapacity) {
		if (!backendOutput || backendFrames == 0 || !resampleScratch || scratchCapacity == 0) {
			return backendFrames;
		}
/*
	// Important: the output FIFO must only pop frames that the resampler will
		// actually consume on this cycle. Otherwise we drop valid samples and hear
		// periodic crackles (data discontinuities).
		//
		// For a seeded streaming resampler, the number of new input frames needed to
		// render `backendFrames` outputs is floor(phase + backendFrames * step).
		// This is the exact number of interpolation-window advances performed in
		// ResamplerState::process().
		const double projected = channel.outputResampler.phase
			+ (static_cast<double>(backendFrames) * channel.outputResampler.step);
		size_t requiredInput = static_cast<size_t>(projected);
		
		// During startup, process() bootstraps up to 4 samples before it can render.
		if (!channel.outputResampler.seeded) {
			requiredInput += 4;
		}
*/
		// Important: pop only frames that the resampler is expected to consume for
		// this backend cycle (phase-aware budgeting avoids data discontinuities).
		size_t requiredInput = estimateRequiredResamplerInputFrames(
			channel.outputResampler,
			backendFrames
		);

		if (requiredInput > scratchCapacity) {
			requiredInput = scratchCapacity;
		}

		const size_t available = channel.fifo.available();
		const size_t popped = (available < requiredInput) ? available : requiredInput;
		if (popped > 0) {
			channel.fifo.pop(resampleScratch, popped);
		}

		size_t consumed = 0;
		size_t produced = 0;
		if (popped > 0) {
			produced = channel.outputResampler.process(
				resampleScratch,
				popped,
				backendOutput,
				backendFrames,
				consumed
			);
		}
		(void)consumed;

		for (size_t i = produced; i < backendFrames; ++i) {
			backendOutput[i] = 0.0f;
		}

		return backendFrames - produced;
	}
}
