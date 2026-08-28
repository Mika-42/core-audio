# Core-Audio AGENTS.md

## Overview
C++ audio library providing a unified abstraction over backend SDKs (ASAL, PipeWire, etc.) using ETSI CMI2.0 `.cppm` modules. Target: C++26.

## Build & Run
- Compiler: `g++ -std=c++26`
- Libraries: pipewire-0.3 (`pkg-config --cflags --libs libpipewire-0.3`) + ALSA (`-lasound`)
- Core source (always compiled): `src/utils/*.cppm`, `src/abstract_backend.cppm`
- Backend impls (currently active): `src/impl/pipewire_impl.cppm`, `src/impl/alsa_impl.cppm`

```bash
# Full build
make

# Debug builds
make CPP_FLAGS="-Wall -Wextra -Werror" DBG_FLAGS="-g -Og"

# Individual tests/examples (same module objects reused)
make EXAMPLE_DIR=/path/to/example

# Clean
make clean
```

## API Usage Pattern
```cpp
import audio.<backend>;  // e.g., audio.jack, audio.pipewire

// Get available devices/channels
auto channels = engine.getChannels();
for(auto& ch : channels) {
    println("{}", ch.name);
}

// Setup
engine.setSampleRate(48000);
engine.setBlockSize(1024);
engine.setCallback(callback_func);

// Pipeline: open → start → do work → stop → close
auto ret = engine.open(device);       // fail-fast, no auto-negotiation
engine.start();
...                                  // callback invoked per block
engine.stop();
ret = engine.close();                // check ok() on errors
```

## Core Abstractions
- `mka::audio::Block`: read-only fields (`blockSize`, `sampleRate`, `inputCount`, `outputCount`), writable `outputs[]`, readonly `inputs[]` (filled by backend)
- `Backend` interface: pure virtual, implemented per-backend (.cppm files use `-fmodules`)
- Backends currently supported: `pipewire`, `alsa` (jack/pipewire bindings in README table not functional yet)

## Development Tips
- Add new feature: edit utils/*.cppm or impl/<backend>_impl.cppm; compile via `make`
- Debugging: `gdb ./app` after running under GDB (`RUN_GDB=1`)
- Callback must match signature exactly: `void func_name(mka::audio::Block& block)`
