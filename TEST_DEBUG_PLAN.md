# AI_STT test and debug plan

This plan is for the current ESP32-S3 firmware in `CODES/CODESPACE/AI_STT_MAIN`.
It separates hardware tests from cloud tests so a wiring fault cannot be mistaken
for a Whisper/API success.

## Test setup

Record these values at the top of every test run:

| Field | Value |
|---|---|
| Board / revision | ESP32-S3 Dev Module / __________ |
| Arduino-ESP32 core | 3.3.11 expected |
| PSRAM | Disabled expected |
| Firmware commit | __________ |
| INMP441 L/R | GND = left; 3V3 = right |
| Wi-Fi AP / RSSI | __________ / __________ dBm |
| Groq model | `whisper-large-v3-turbo` |
| Speaker / amplifier | __________ |

Use a 115200 baud serial capture with timestamps. Do not paste API keys into
serial logs or repository files.

## Pass/fail tests

### T01 — Build and boot smoke test

**Steps:** Copy `AI_STT_CONFIG_DEFAULT.h` to `AI_STT_CONFIG.h`, fill local
credentials, compile for `ESP32S3 Dev Module` with PSRAM disabled, flash, and
open the serial monitor.

**Expected:** The sketch builds with Arduino-ESP32 3.3.x, prints
`SpeechToText - Groq Whisper`, reaches `[wifi] ...`, then prints
`[mic] ... -> started`. No reboot loop, Guru Meditation, or `I2S INIT FAILED`.

**Failure clues:**

- `ESP_I2S.h` missing: wrong/old ESP32 core.
- Blank monitor: wrong USB CDC/serial-port setting.
- `I2S INIT FAILED`: check board target, PSRAM setting, and GPIO wiring.

### T02 — INMP441 wiring and channel test

Run either microphone test sketch first, with the microphone physically quiet,
then clap or speak 10 cm from the capsule. Capture at least 20 peak lines.

**Expected:** Quiet peaks remain substantially below speech peaks; speech causes
repeatable increases. With L/R grounded, the left-channel configuration is used.
Swap L/R only as a diagnostic: the selected channel should change from signal to
near-silence, not merely reduce volume.

**Check wiring:** VDD=3V3, GND=GND, SD=GPIO33, SCK=GPIO26, WS=GPIO25. Never
connect the INMP441 to 5 V.

**Failure clues:** Constant zero means power, SD, WS, SCK, or channel selection.
Constant near-full-scale means floating/miswired data or wrong I2S format.
Speech on only one L/R position indicates channel selection is working.

### T03 — Sample-rate and capture-duration test

Hold BOOT for a measured 3.0 seconds, release, and capture the `[rec]` line.
Repeat three times.

**Expected:** `total` is approximately `96000` bytes (16,000 samples/s × 2
bytes/sample × 3 s), within the button/debounce timing tolerance. The printed
duration is close to 3.0 s and never exceeds `MAX_SECONDS`.

**Failure clues:** Approximately half the expected byte count indicates dropped
reads or a channel/interleave mistake. Twice the expected count indicates a WAV
sample-width mismatch. A transcript that sounds twice as fast strongly suggests
8 kHz data labeled as 16 kHz.

### T04 — Audio level, filter, and clipping test

Make three recordings: quiet room, normal speech at 30 cm, and loud speech at
10 cm. Record the printed `peak` for each.

**Expected:** Quiet room is below `SILENCE_PEAK` only when genuinely silent;
normal speech is above it; loud speech does not stay at 32767 for long. A normal
speech peak should have useful margin below full scale.

**Failure clues:** Every recording reports `no signal`: inspect the mic path
before changing the threshold. Frequent 32767 peaks indicate `MIC_GAIN` or the
analog level is too high and will degrade transcription. Speech consistently
below 300 indicates gain, wiring, or the threshold needs investigation.

### T05 — Wi-Fi and API happy path

With a known-good network, make five short recordings using the same phrase.
Capture `[wifi]`, `[rec]`, `[stt]`, HTTP errors, and the final `You said:` line.

**Expected:** Wi-Fi obtains an IP, each upload returns HTTP 200, `[stt]` is
finite and normally below the project latency target, and the body is plain
text without HTTP chunk-size markers.

**Failure clues:** HTTP 401/403 means the key/configuration; 429 means rate
limiting; 5xx or connect failure means network/service path. Chunk-size strings
in the transcript indicate broken response chunk parsing. A valid-looking
`Thank you` on a disconnected mic is not a pass: T04 must pass first.

### T06 — Latency measurement

For each of five recordings, capture the time of button release and the `[stt]`
line. The current `[stt]` value measures from the final audio sample to the end
of the response, not from button press.

Report median and worst case, not only one best run. Also report recording time,
RSSI, response status, and whether the idle TLS connection was reused.

**Expected:** No unbounded wait. A slow or unavailable service must fail within
the implemented timeout and return to the idle loop.

### T07 — Network and recovery tests

Run each case separately and power-cycle only when the expected recovery requires
it:

1. Boot with the AP unavailable, then restore the AP.
2. Disconnect Wi-Fi while idle, then restore it.
3. Disconnect Wi-Fi during an upload.
4. Let the preconnected idle TLS socket sit until the server closes it.
5. Enter an invalid API key.
6. Hold BOOT longer than 15 seconds.

**Expected:** The device remains responsive and reports a useful error. For transient network/server cases, a later attempt can work without a power cycle; an invalid API key must continue to fail until the firmware configuration is corrected. A held button stops at `MAX_SECONDS`.

**Known current risk:** `setup()` waits forever in the Wi-Fi loop, so case 1
currently fails the recovery requirement until a bounded retry/reconnect state is
implemented. `ensureLink()` uses `setInsecure()`, so transport encryption is used
but the Groq server certificate is not authenticated.

### T08 — HTTP framing and WAV compatibility

Run a successful upload while capturing the request with a controlled HTTPS test
endpoint or a host-side mock that can return: fixed `Content-Length`, chunked
body split across many packets, empty body, HTTP 401, HTTP 429, HTTP 500, and a
delayed response.

**Expected:** Chunk boundaries never appear in the returned transcript; non-200
responses are reported; delayed responses time out; the request ends with the
multipart boundary and zero-size chunk.

**Compatibility risk to verify:** the sketch streams a WAV with RIFF/data sizes
set to `0xFFFFFFFF`. Confirm the current transcription endpoint accepts this
format. If it does not, buffer the recording or patch the WAV sizes before upload.

### T09 — MAX98357A and 4-ohm speaker (not implemented)

This is a blocked integration test for the current repository. There is no
MAX98357A pin configuration, I2S TX path, decoded audio source, volume control,
or playback state machine in the production sketch.

When playback is added, test in this order:

1. Amplifier power and common ground; never drive the speaker directly from an
   ESP32 GPIO.
2. I2S BCLK/WS/DIN pin mapping and left/right slot selection.
3. Low-volume tone, then speech playback, with a current-limited supply.
4. 4-ohm speaker thermal/current behavior for a 30-second run.
5. Concurrent capture/upload/playback and recovery after I2S underrun.

**Expected acceptance:** clean audio with no sustained clipping, no ESP32 reset,
no amplifier overheating, and a defined behavior when Wi-Fi/API fails.

## Fault isolation order

1. Power and wiring (T02), then raw I2S signal (T02).
2. Sample count/rate and clipping (T03–T04).
3. Wi-Fi and HTTP status (T05, T07–T08).
4. Recognition quality and latency (T05–T06).
5. Playback only after a transcript path is stable (T09).

## Run log template

```text
Date/time:
Firmware commit:
Board/core/PSRAM:
Test ID:
Wi-Fi RSSI:
Serial excerpt:
Measured values:
Result: PASS / FAIL / BLOCKED
Likely fault:
Next action:
```

## Current implementation findings

- The production path is microphone capture plus Groq Whisper text response only;
  MAX98357A/4-ohm speaker output is absent.
- Wi-Fi startup has no timeout and can prevent all later tests from running.
- TLS certificate verification is disabled with `setInsecure()`.
- The firmware assumes the server accepts streaming WAV headers with unknown
  RIFF/data lengths; this needs an endpoint compatibility test.
- The idle connection is reused, but there is no explicit stale-socket retry
  after a failed first write.
- The existing microphone sketches use the legacy I2S API and should be treated
  as diagnostic sketches, not proof that the production `ESP_I2S.h` path works.
