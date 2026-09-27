# ESP32S AI Speech-to-Text

A prototype speech-to-text system built using an **ESP32**, **INMP441 I2S microphone**, and a cloud speech backend.

Hold the BOOT button to record speech, release it to stop, and the ESP32 sends the audio to Whisper for transcription. The resulting text is displayed in the Serial Monitor.

## Project Goals

The goal of this project is to develop a standalone, ESP32-based AI voice assistant capable of capturing speech, communicating with cloud-based AI services, and responding to the user through a speaker.

This project will be interfaced with other AI features, such as AI vision.

## Firmware architecture

The checked-in main sketch is an ESP32-S3/Groq proof of concept and is not yet the production firmware for the original ESP32-S 38-pin CP2102 board. The hardware, audio, buffering, playback, and secure backend baseline is documented in [docs/firmware-architecture.md](docs/firmware-architecture.md).

Provider API keys must remain on the backend and must never be compiled into device firmware.
