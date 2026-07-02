module;
#include <string>
export module audio.error;

using namespace std::literals;

export namespace mka::audio {

	enum class Error {
		None,
	   	GenericError,
		DeviceOpenFailed,
		SetupHardwareParameterFailed,
		PollSetupFailed,
		HardwareSetupFailed,
		PollDescriptorsFailed,
		XRun,
		WouldBlock,
		AlreadyExists,
		InvalidArgument,
		OutOfRange,
		NotFound,
	};

	struct Result {
		Error		error;
		std::string	message;

		bool ok() const {	
			return error == Error::None;
		}

		explicit operator bool() const {
			return error == Error::None;
		}
		
		template <typename T> Result then(T&& f) const {
			if(!*this) return *this;
			return f();
		}
		
		template <typename T> Result onError(T&& f) const {
			if(*this) return *this;
			f(*this);
			return *this;
		}
	};

	inline constexpr Result Ok { Error::None, ""s };
	inline const Result Fail { Error::GenericError, "generic error occured."s };

}
