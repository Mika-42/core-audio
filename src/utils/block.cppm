module;
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <bit>
export module audio.block;

export namespace mka::audio {

	// Bloc audio planar, stockage brut alloué UNE SEULE FOIS à la
	// construction (configure()), jamais réalloué. Pas de conteneur STL :
	// pointeurs nus possédés directement par cet objet, libérés dans le
	// destructeur. Copie/déplacement interdits par défaut pour éviter une
	// double-libération accidentelle (voir move explicite ci-dessous si
	// nécessaire côté BlockChain).
	class Block {
	public:
		Block() = default;

		~Block() {
			release();
		}

		Block(const Block&) = delete;
		Block& operator=(const Block&) = delete;

		// Déplacement explicite autorisé (nécessaire pour que BlockChain
		// puisse construire un tableau de Block sans copie), transfère la
		// propriété du stockage brut sans réallocation.
		Block(Block&& other) noexcept
			: storage_(other.storage_), 
			channelPtrs_(other.channelPtrs_),
			channelCount_(other.channelCount_), 
			frameCount_(other.frameCount_),
			validFrames(other.validFrames),
			cursor(other.cursor) {
			other.storage_      = nullptr;
			other.channelPtrs_  = nullptr;
			other.channelCount_ = 0;
			other.frameCount_   = 0;
			other.cursor = 0;
		}

		Block& operator=(Block&& other) noexcept {
			if (this != &other) {
				release();
				storage_      = other.storage_;
				channelPtrs_  = other.channelPtrs_;
				channelCount_ = other.channelCount_;
				frameCount_   = other.frameCount_;
				validFrames    = other.validFrames;
				cursor			= other.cursor;
				other.storage_      = nullptr;
				other.channelPtrs_  = nullptr;
				other.channelCount_ = 0;
				other.frameCount_   = 0;
			}
			return *this;
		}

		// Alloue le stockage pour channelCount canaux de frameCount
		// échantillons chacun. À appeler uniquement à la construction de
		// la BlockChain, jamais depuis un thread temps réel. Remplace tout
		// stockage précédent (libère l'ancien avant réallocation).
		void configure(size_t channelCount, size_t frameCount) {
			release();
			channelCount_ = channelCount;
			frameCount_   = frameCount;

			// Stockage planar contigu : un seul bloc mémoire brut, canaux
			// adressés par offset. new[] lève std::bad_alloc en cas
			// d'échec, jamais retourné nullptr silencieusement.
			storage_     = new float[channelCount * frameCount];
			channelPtrs_ = new float*[channelCount];
			for (size_t c = 0; c < channelCount; ++c) {
				channelPtrs_[c] = storage_ + c * frameCount;
			}
			clear();
		}

		[[nodiscard]] size_t channels() const noexcept { return channelCount_; }
		[[nodiscard]] size_t capacityFrames()   const noexcept { return frameCount_; }

		[[nodiscard]] float* channel(size_t index) noexcept { return channelPtrs_[index]; }
		[[nodiscard]] const float* channel(size_t index) const noexcept { return channelPtrs_[index]; }

		// Réinitialisation temps réel : remise à zéro du contenu déjà
		// alloué (std::memset, pas de nouvelle allocation). Safe RT.
		void clear() noexcept {
			if (storage_) {
				std::memset(storage_, 0, channelCount_ * frameCount_ * sizeof(float));
			}
			validFrames = 0;
			cursor = 0;
		}

		size_t validFrames    = 0;
		size_t cursor		  = 0;

	private:
		void release() noexcept {
			delete[] storage_;
			delete[] channelPtrs_;
			storage_     = nullptr;
			channelPtrs_ = nullptr;
		}

		float* storage_          = nullptr;
		float** channelPtrs_     = nullptr;
		size_t channelCount_ = 0;
		size_t frameCount_   = 0;
	};

	class BlockView {
		public:
			BlockView(Block& b) : block(b) {}

			[[nodiscard]] size_t frames() const noexcept { return block.capacityFrames(); }
			[[nodiscard]] size_t channels() const noexcept { return block.channels(); }
			[[nodiscard]] float* channel(size_t index) noexcept { return block.channel(index); }
			[[nodiscard]] const float* channel(size_t index) const noexcept { return block.channel(index); }
			void clear() noexcept { block.clear(); }
		private:
			Block& block;
	};
}

