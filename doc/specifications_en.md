# MKA Core Audio

## 1. Purpose

MKA Core Audio is a real-time audio abstraction layer designed to provide a unified interface to audio systems across different platforms and backends.

It abstracts backend-specific APIs while exposing a small, predictable and backend-independent interface to higher-level components.

Core Audio is responsible for transporting audio between the application and the underlying audio system. It does not process or interpret the audio.

## 2. Objectives

Core Audio must:

* provide a common interface across different audio backends;
* enumerate available audio devices;
* expose device capabilities;
* allow the application to select a stream configuration;
* open and manage audio streams;
* provide real-time audio buffers;
* remain independent from any specific backend API;
* provide deterministic and real-time-safe behavior.

Core Audio must remain small, stable and predictable.

## 3. Conceptual Model

Core Audio uses three main concepts:

```text
Backend
   │
   ├── Devices
   │
   └── Stream
        ├── Input Channels
        └── Output Channels
```

### Device

A device is a selectable audio resource or endpoint exposed by a backend.

A device does not necessarily represent physical hardware. The backend maps its native concepts to the generic `Device` model.

### Stream

A stream is an active audio session opened for a specific device and configuration.

The stream configuration is immutable during the lifetime of the stream.

Changing the configuration requires closing the current stream and opening a new one.

### Channel

A channel represents one audio signal transported by a stream.

Backend-specific concepts such as ALSA PCM channels, PipeWire ports, WASAPI channels or CoreAudio elements must not leak into the Core Audio public interface.

## 4. Device Discovery and Capabilities

Device discovery and stream opening are two separate operations.

The application first retrieves the available devices:

```cpp
getDevices()
```

It can then query the capabilities of a selected device:

```cpp
getCapabilities(device)
```

Capabilities may include:

* supported sample rates;
* supported sample formats;
* supported input/output channel counts;
* supported buffer sizes or buffer-size ranges;
* other constraints required to open a stream.

Capabilities describe what the backend can support. They do not represent a stream configuration.

## 5. Stream Configuration

The application is responsible for selecting the desired stream configuration.

For example:

```cpp
StreamConfig {
    device,
    sampleRate,
    bufferSize,
    format,
    inputChannels,
    outputChannels
}
```

The configuration represents an explicit request made to the backend.

### Strict Configuration Rule

`open(config)` must request the specified configuration exactly.

Core Audio must not silently modify, approximate or automatically negotiate the requested configuration.

If the backend cannot provide the requested configuration, `open()` must fail with an appropriate error.

For example:

```text
Requested:
    48000 Hz
    Float32
    256 frames
    2 input channels
    2 output channels

Backend:
    Supported

Result:
    OPEN
```

If the backend cannot provide that configuration:

```text
Requested:
    48000 Hz
    Float32
    256 frames

Backend:
    Configuration unsupported

Result:
    ERROR
```

The backend must not silently replace it with, for example, `44100 Hz` or `512 frames`.

This makes stream creation deterministic and prevents higher-level components from unknowingly operating with a different audio configuration.

The application can use `getCapabilities()` before calling `open()` to determine whether the requested configuration is supported.

The backend must still validate the configuration during `open()`, since capabilities may change between discovery and stream creation.

## 6. Stream Lifecycle

The minimum stream lifecycle is:

```text
CLOSED
   │ open()
   ▼
OPEN
   │ start()
   ▼
RUNNING
   │ stop()
   ▼
OPEN
   │ close()
   ▼
CLOSED
```

The minimum public interface is:

```cpp
getDevices()
getCapabilities(device)

open(config)
start()
stop()
close()
```

Stream configuration cannot be modified while the stream is open.

A configuration change requires:

```text
stop()
   ↓
close()
   ↓
open(newConfig)
   ↓
start()
```

Any seamless hot-swap or advanced reconfiguration mechanism is the responsibility of the `AudioEngine`, not Core Audio.

## 7. Audio Buffer

Core Audio provides audio buffers to higher-level components.

A generic buffer may be represented as:

```cpp
struct AudioBuffer {
    float* const* input;
    float* const* output;

    uint32_t inputChannels;
    uint32_t outputChannels;
    uint32_t frames;
};
```

The exact internal representation may vary between backends.

Core Audio is responsible for translating backend-native audio data into the generic buffer representation.

Backend-native sample formats may be converted when required by the abstraction.

Core Audio must not impose a fixed DSP block size. The number of frames provided by the backend may vary.

Fixed-size DSP processing belongs to the DSP Engine.

## 8. Real-Time Constraints

The audio callback must be real-time safe.

The real-time execution path must avoid:

* dynamic memory allocation;
* deallocation;
* blocking mutexes;
* filesystem access;
* network operations;
* potentially blocking system calls;
* unbounded operations;
* heavy logging;
* operations with unpredictable execution time.

Backend-specific real-time requirements must be respected by each backend implementation.

## 9. Core Audio Responsibilities

Core Audio is responsible for:

* device enumeration;
* capability discovery;
* stream configuration;
* stream creation;
* stream lifecycle management;
* audio input/output transport;
* audio buffer abstraction;
* sample-format abstraction and conversion where required;
* backend-independent error reporting;
* real-time callback handling;
* backend isolation.

## 10. Out of Scope

Core Audio must not handle:

* DSP processing;
* DSP block scheduling;
* plugins;
* VST/AU processing;
* mixers;
* audio graphs;
* nodes;
* tracks;
* buses;
* logical audio routing;
* transport;
* timeline;
* automation;
* MIDI;
* project management;
* UI;
* networking;
* DAW business logic.

Physical or virtual audio routing belongs to a higher-level audio component.

DSP processing and DSP scheduling belong to the DSP Engine.

## 11. Separation of Responsibilities

The architecture follows this principle:

```text
DAW Engine
    ↓
DSP Engine
    ↓
Audio Routing
    ↓
Audio Engine
    ↓
Core Audio
    ↓
Backend
    ↓
Operating System / Hardware
```

Responsibilities are separated as follows:

**Core Audio**

> Abstracts the backend and transports audio.

**Audio Engine**

> Orchestrates real-time audio execution and higher-level audio operations.

**Audio Routing**

> Determines how physical and virtual audio channels are connected.

**DSP Engine**

> Processes audio and manages DSP execution.

**DAW**

> Determines what the system should do.

## 12. Backend Independence

Core Audio must not expose backend-specific types or concepts.

The public interface should use generic concepts such as:

```cpp
DeviceID
DeviceInfo
DeviceCapabilities
StreamConfig
AudioBuffer
SampleFormat
Error
```

Backend implementations may internally use:

* ALSA;
* PipeWire;
* JACK;
* WASAPI;
* CoreAudio;
* or other native APIs.

These implementation details must remain isolated from the rest of the application.

## 13. Error Handling

Errors must be explicit and deterministic.

Examples include:

```cpp
None
InvalidArgument
NotFound
AlreadyExists
UnsupportedConfiguration
DeviceOpenFailed
HardwareSetupFailed
PollSetupFailed
PollDescriptorsFailed
WouldBlock
XRun
```

In particular, an unsupported stream configuration must result in an explicit error rather than an automatic configuration change.

## 14. Acceptance Criteria

A Core Audio implementation is considered compliant if it:

* enumerates available devices;
* exposes their capabilities;
* allows an application to construct an explicit stream configuration;
* validates that configuration when opening a stream;
* opens the stream only with the requested configuration;
* fails explicitly when the requested configuration is unsupported;
* supports `open/start/stop/close`;
* provides real-time audio buffers;
* does not impose a fixed DSP block size;
* respects real-time execution constraints;
* keeps backend-specific concepts out of the public API;
* allows multiple backends to implement the same interface;
* requires stream recreation when its configuration changes.

## 15. Architectural Principle

> **Core Audio must remain small, stable, deterministic and predictable.**

The backend transports audio.

Core Audio abstracts the backend.

The Audio Engine orchestrates real-time execution.

The DSP Engine processes audio.

The DAW decides what should be processed.

Any functionality that is not directly related to device discovery, capability discovery, stream configuration, stream lifecycle or real-time audio transport belongs to a higher-level component.
