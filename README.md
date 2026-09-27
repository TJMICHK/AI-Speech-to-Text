# ESP32S AI Speech-to-Text

A simple speech-to-text system built using an **ESP32S**, **INMP441 I2S microphone**, and **Groq Whisper API**.

Hold the BOOT button to record speech, release it to stop, and the ESP32 sends the audio to Whisper for transcription. The resulting text is displayed in the Serial Monitor.

## Project Goals

The goal of this project is to develop a standalone, ESP32-based AI voice assistant capable of capturing speech, communicating with cloud-based AI services, and responding to the user through a speaker.

This project will be interfaced with other AI features, such as AI vision.
