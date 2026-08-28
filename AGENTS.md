# mka-core-audio — Agent Instructions

## How this repo works (architecture is not obvious)

- **Language**: C++26, using Gallium-style module files (`.cppm`) with GCC `-fmodules` flag. Not Rust or Zig syntax but semantically similar.
- **Build system**: Makefile only (no CMake, no build.rs). Compiler: `g++ -std=cXX6`.
- **Package boundary**: Single monorepo; source lives under `src/`. The exported modules are in `audio.*`.cppm files.
- **Binary**: Executable is `./app` built from `main.cpp` + module objects in `obj/`.

## Developer commands (verify these first)

```bash
# Compile the app with debug symbols and error-on-warning enabled
g++ -std=c++26 \
  -Wall -Wextra -Werror -Wpedantic \
  -fmodules \
  $(pkg-config --cflags --libs libpipewire-0.3) \
  -lasound \
  src/main.cpp src/utils/*.cppm src/impl/pipewire_impl.cppm -o app

# Full rebuild (includes obj/ cleanup)
make clean && make all

# Build examples (they use the same object files)
make examples
```

**Required order**: build modules → link main. Module objects live in `obj/*.o`.

## Core abstractions (not documented in prose)

- **`mka::audio::Backend`**: Abstract base class; each concrete backend (`PipeWire`, future ALSA impl) inherits from it and implements pure virtual methods: `getDevices()`, `getCapabilities(id)`, `open(cfg)`, `start()`, `stop()`, `close()`.
- **State lifecycle**: `Backend::State` enum values are `Closed | Open | Running`. Once `close()` is called, no further state changes are allowed.
- **Callback contract**: Audio callbacks must be `void(processNextBlock(const BlockView& input, BlockView& output))`. Callback runs in realtime (RT) thread from PipeWire; **do not** call any backend control methods (`open/start/`...) from callback. Backends may use a check macro like:

  ```cpp
  auto ret = operation();
  if (!ret.ok()) { std::println("error::{}", ret.message); return -1; }
  ```

- **Device discovery**: `Backend.getDevices()` returns `std::vector<std::string>`. Use `getCapabilities(deviceId)` to inspect supported samplerates, buffer sizes, and formats before calling `open()`.

## Platform quirks (differ from defaults)

- **PipeWire only** in current source (`pipewire_impl.cppm`); PipeWire 0.3+ library via `pkg-config libpipewire-0.3`.
- **Auto-connect**: The impl uses `pw_stream` with auto-connect; device IDs are human-readable but the driver does NOT create a stream based on them after `open()`. Routing MUST use `routePort()` explicitly after `open()`.
- **Port naming**: Ports are named `"output_AUXn"` (outputs) and `"input_AUXn"` (inputs). Do not hardcode alternative names.
- **Formats**: Use enum `SampleFormat { Int16, Int24, Int32, Float32, Float64 }`. Most backends advertise `Float32` by default.

## Testing and verification (not obvious from README)

- No unit tests in the repo; test by compiling and running the example code (commented in main.cpp).
- Example workflow: discover devices → pick one → enumerate ports with `enumeratePorts()` → route via `routePort("mka_audio_out", ...)` → set callback via `setCallback()` → start/stop/close.
- Use `gdb -ex run --args ./app` to attach a debugger after building the app binary.

## Repository structure (file system ≠ logical boundaries)

```
src/            # Gallium-style source modules
├─ main.cpp     # Entry point; commented examples show typical usage patterns
├─ abstract_backend.cppm   # Defines mka::audio::Backend class and Device abstract class
├─ impl/        # Concrete backend implementations (pipewire_impl.cppm, alsa_backend.cppm placeholder)
│   ├─ pipewire_impl.cppm     (main impl, ~860 lines of PipeWire 0.3 bindings)
│   └─ alsa_backend.cppm      # Currently empty stub
└─ utils/       # Shared utilities including BlockView structure
├─ gcm.cache/   # Cache directory; safe to rebuild or ignore during clean
├─ Makefile     # Only build tool (use make targets: all, examples)

obj/            # Compiled object files (*.o); removed by `make clean`
build/          # Optional build directories managed by Makefile
app binary      # Output of make; runs the audio backend demonstration
```

- **Do not** assume a file like utils/\*.cppm belongs to a single module; they may be imported across multiple `.cppm` files. Verify with search in each `.cppm`.

## Environment requirements (non-obvious)

- **Compiler**: GCC 13+ required for `-std=c++26`. Check `g++ --version`.
- **Libraries** via pkg-config: `libpipewire-0.3` + `libasound` on Linux. `pkg-config --cflags --libs libpipewire-0.3` must return flags/libs. PipeWire not present → build fails or only stub impls.
- **Audio device**: One physical input/output device is required to test actual audio flow (commented code in main.cpp shows interactive selection).

## Style and workflow conventions (deviates from defaults)

- `block.frames()` / `block.channels() / block.channel(n)` are the public API for `mka::audio::BlockView`.
- Use `std::println` over streams; prefer error reporting via Result type or check macro.
- Code is heavily commented in French/Gallianized English; do not remove comments—they document design decisions like "pw_stream vs pw_filter" and "no auto-connect".

## What NOT to assume

- Not Zig/Rust syntax (`.cppm` = `.c++` + `module` keyword).
- No Cargo/pytest/pytask/npm; build via Makefile commands only.
- The commented sections in `main.cpp` are minimal working examples—expand them as needed.
- `alsas_backend.cppm` is currently empty; no ALSA implementation exists yet.

## Where the real info lives (if you have more questions)

- Concrete backend details: `/src/impl/pipewire_impl.cppm` (~860 lines). Search for `pw_stream_create()`, `routePort()`, `setCallback()` patterns there.
- BlockView usage: `/src/abstract_backend.cppm` and `/src/utils/*.cppm`.
- Compiler flags used everywhere: `Makefile` uses `-std=c++26 -fmodules`.

(End of file)
