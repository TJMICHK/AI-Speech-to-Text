#pragma once

// IN ORDER TO USE THIS FILE, PLEASE RENAME TO
// "AI_STT_CONFIG.h"
// THEN FILL IN THE 3 FIELDS BELOW

#define SEED_GROQ_KEY "###"
#define SEED_WIFI_SSID "###"
#define SEED_WIFI_PASS "###"

#define SAMPLE_RATE   16000   // Whisper's native rate; higher just wastes upload
#define MAX_SECONDS   15      // hard stop, so a stuck button cannot record forever
#define SILENCE_PEAK  300     // below this, treat the clip as silence
#define HP_CUTOFF_HZ  120     // high-pass corner, removes DC and rumble
#define MIC_GAIN      6       // digital gain applied after filtering

#define PIN_BUTTON    0       // BOOT, active LOW
#define PIN_MIC_SD    33
#define PIN_MIC_SCK   26
#define PIN_MIC_WS    25

#define GROQ_HOST     "api.groq.com"
#define STT_MODEL     "whisper-large-v3-turbo"
#define BOUNDARY      "----speechtotext"