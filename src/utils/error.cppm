export module audio.error;

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
		const char*	message;

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

	inline constexpr Result Ok { Error::None, nullptr };
	inline constexpr Result Fail { Error::GenericError, "generic error occured." };

}
