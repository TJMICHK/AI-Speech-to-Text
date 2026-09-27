# AI_STT firmware architecture

Status: design baseline for the original ESP32-S 38-pin CP2102 board.

## Scope and assumptions

- The target is the original ESP32 (not ESP32-S3), with a 38-pin CP2102 USB/UART board.
- The microphone is an INMP441, powered from 3.3 V. Its L/R pin is tied to GND for the left slot.
- The amplifier is a MAX98357/MAX98357A. It accepts I2S digital audio and drives the 4-ohm speaker; do not connect the speaker to an ESP32 GPIO.
- Wi-Fi is the transport. The device is not trusted with an OpenAI, Groq, or other provider secret.
- The backend owns provider credentials and translates device requests to OpenAI services.
- GPIO numbers below are proposed defaults and must be checked against the exact board's silkscreen and any future peripherals.

## Proposed wiring

| Signal | ESP32 GPIO | Device | Notes |
| --- | ---: | --- | --- |
| I2S BCLK/SCK | 26 | INMP441 SCK, MAX98357 BCLK | Shared clock |
| I2S WS/LRCLK | 25 | INMP441 WS, MAX98357 LRC | Shared clock |
| Mic data | 33 | INMP441 SD | Input only |
| Amp data | 22 | MAX98357 DIN | Output only |
| Mic L/R | — | GND | Select left channel |
| Amp SD/EN | 21 | MAX98357 SD/EN | Optional software mute/enable |
| Grounds | GND | All modules | Common ground is required |

The ESP32 is the I2S clock master. Capture and playback should use one I2S peripheral in full-duplex mode with the shared BCLK/WS lines and separate data-in/data-out pins. If the selected Arduino-ESP32 core cannot provide stable full-duplex operation through its high-level API, use the ESP-IDF I2S driver for this board rather than silently falling back to two independently clocked buses.

## Firmware boundaries

The firmware should be split into these small modules:

1. `board_config`: pins, sample rates, button, watchdog, and feature flags.
2. `audio_engine`: I2S RX/TX, mono capture, 16-bit PCM conversion, playback queue, gain, mute, and underrun/overrun counters.
3. `session_manager`: button/voice activity state machine and session limits.
4. `transport`: Wi-Fi reconnect, TLS validation, request framing, bounded timeouts, and retry policy.
5. `backend_protocol`: device authentication, request IDs, audio chunks, transcript/events, and error mapping.
6. `diagnostics`: serial logs, counters, RSSI, heap watermark, and reset reason.

The main loop should schedule these components without blocking on Wi-Fi or audio. Audio DMA must continue draining while network operations are slow.

## Audio pipeline

Capture format should be 16 kHz, mono, signed 16-bit PCM for speech recognition. The INMP441 transmits 24-bit samples in 32-bit I2S slots; conversion belongs at the audio boundary, with explicit slot/channel selection and a peak/RMS meter for diagnostics. Preserve the current high-pass filtering only after measuring the actual microphone and enclosure noise; hard-coded gain of 6 can clip speech.

Use bounded buffers:

- DMA buffers: small enough for low latency, large enough to tolerate scheduler jitter.
- A ring buffer between I2S and transport: about 200–500 ms of PCM as the initial target.
- A separate playback ring buffer for decoded 16-bit PCM from the backend.

Do not depend on an open-ended WAV file with `0xFFFFFFFF` sizes as the device/backend contract. Either send framed PCM chunks with a session ID, or buffer to a known-length file and send a valid WAV. Framed PCM avoids requiring roughly 480 KB of RAM for a 15-second clip and supports incremental recognition.

Playback must include a bounded queue, amplifier mute during startup/shutdown, and an underrun policy (insert silence and increment a counter). The MAX98357 is a power amplifier, not a DAC exposed as analog audio; its DIN/BCLK/LRC pins must receive I2S.

## Secure network design

The current `WiFiClientSecure::setInsecure()` and firmware-defined provider key are development-only and must not ship. The production flow is:

```text
ESP32 -- TLS + device credential --> AI_STT backend -- provider TLS + server secret --> OpenAI
```

The backend should expose a narrow endpoint for audio/session events, enforce payload and duration limits, authenticate each device, and return only the events the device needs. Use a per-device credential or short-lived signed token; support revocation and rotation. Validate the backend certificate using a pinned public key or a maintained CA certificate. Never log tokens, audio contents, or provider responses containing sensitive data.

On the ESP32, Wi-Fi behavior must include connection timeouts, exponential backoff, a maximum offline queue, and a clear user-visible error state. TLS and HTTP operations must have finite deadlines so the audio task cannot starve.

## State machine

`BOOT` -> `WIFI_CONNECTING` -> `READY` -> `CAPTURING` -> `UPLOADING`/`PROCESSING` -> `PLAYING` -> `READY`.

Any state can enter `ERROR` with a reason code and recover to `WIFI_CONNECTING` or `READY`. Button handling should be debounced and independent of network latency. A watchdog-safe session limit is required for stuck buttons or broken connections.

## Implementation order

1. Correct the board target and pin configuration; add a compile-time board profile for original ESP32 versus ESP32-S3.
2. Replace the legacy one-way I2S test with a full-duplex audio-engine test: capture peak/RMS, generate a short tone, and verify speaker output.
3. Add ring buffers and prove capture/playback remain lossless under synthetic network stalls.
4. Implement the backend protocol against a local/mock endpoint before adding a real provider.
5. Add certificate validation and device authentication; remove all provider keys from sketches and headers.
6. Add streaming transcription and response playback, then test Wi-Fi loss, TLS failure, buffer pressure, brownouts, and repeated sessions.

## Current repository risks

- `CODES/CODESPACE/AI_STT_MAIN/AI_STT_MAIN.ino` documents an ESP32-S3 and uses S3-specific `ESP_I2S.h` APIs, so it is not the baseline for the stated original ESP32 board.
- `AI_STT_CONFIG_DEFAULT.h` encourages putting Wi-Fi and provider secrets in a copied header. Wi-Fi credentials may be locally configured, but provider credentials must be removed from firmware entirely.
- `setInsecure()` encrypts traffic without authenticating the server and is vulnerable to man-in-the-middle attacks.
- The current sketch has no MAX98357 pin, playback driver, response audio format, or speaker safety/mute behavior.
- Blocking Wi-Fi connection and HTTP reads can starve audio and make recovery unreliable.
- The current streaming WAV format uses unknown RIFF/data lengths. It should be replaced by framed PCM or a valid finalized WAV contract.

