module;
#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>
export module audio.routing_table;

export namespace mka::audio {

	/*
	 * @exemple 
	 *
	 * ```c++
	 * // physical -> virtual
	 * routing.input(0).link(3);		// add link
	 * routing.input(0).unlink();		// rm link
	 * routing.input(0).get();			// 3
	 * 
	 * // virtual -> physical
	 * routing.output(0).link(2);		// add link
	 * routing.output(0).link(3);		// add link
	 * routing.output(0).link(7);		// add link
	 * routing.output(0).unlink(7);		// rm link
	 * routing.output(0).get();			// [2, 3]
	 * */


	class OutputRoute {
	public:	
		void link(size_t port) {
			if (std::find(route_.begin(), route_.end(), port) == route_.end()) {
				route_.push_back(port);
			}
		}

		void unlink(size_t port) {
			std::erase(route_, port);
		}

		std::span<const size_t> get() const {
			return route_;
		}

	private:
		std::vector<size_t> route_;
	};

	class InputRoute {
	public:
		void link(size_t port) {
			port_ = port;
		}

		void unlink() {
			port_.reset();
		}

		std::optional<size_t> get() const {
			return port_;
		}

	private:
		std::optional<size_t> port_;
	};

	class RoutingTable {
		public:
			RoutingTable(size_t channels) : channels_(channels), input_(channels_), output_(channels_) {}

			size_t channels() const { return channels_; }

			OutputRoute& output(size_t port) {
				return output_.at(port);
			}

			const OutputRoute& output(size_t port) const {
				return output_.at(port);
			}
			
			InputRoute& input(size_t port) {
				return input_.at(port);
			}

			const InputRoute& input(size_t port) const {
				return input_.at(port);
			}

		private:
			size_t channels_;
			std::vector<InputRoute> input_;
			std::vector<OutputRoute> output_;
	};
}
