#include "../../hal/audio_hal.h"
#include "board.h"
#include "audio_samples.h"   // generated: sampled themes (16 kHz/16-bit/mono PCM)
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <driver/i2s_std.h>

// Notification chimes for the C6 AMOLED-2.16. Audio path:
//   ESP32-C6 I2S0 (TX) --> ES8311 codec --> speaker amp (powered by AXP2101
//   ALDO2, already enabled in board_init) --> onboard speaker.
//
// We play short notification sounds — either synthesized sine chimes (the
// "Retro" theme) or embedded PCM clips (the sampled themes, see
// audio_samples.h) — so the codec runs DAC-only at 16 kHz / 16-bit / mono. The
// ES8311 register sequence below mirrors the
// Espressif es8311 component's default DAC bring-up with MCLK supplied by the
// I2S peripheral (MCLK = 256 * Fs). Hand-rolled rather than pulling in a codec
// library — same no-vendored-dep stance as the touch driver.

#define SAMPLE_RATE   16000
#define MCLK_MULT     256          // ES8311 expects MCLK = 256 * Fs here

// ---- ES8311 register writes over I2C ------------------------------------
static bool es8311_w(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es8311_init(void) {
    // Sequence follows the Espressif esp-bsp es8311 component + Waveshare BSP
    // for MCLK supplied externally at 256*Fs, 16 kHz, 16-bit, DAC playback.
    // Reset, then power-on.
    if (!es8311_w(0x00, 0x1F)) { Serial.println("ES8311 not responding"); return false; }
    delay(20);
    es8311_w(0x45, 0x00);
    es8311_w(0x01, 0x3F);   // CLK_MANAGER: enable all clocks (MCLK from MCLK pin)
    es8311_w(0x00, 0x80);   // slave mode, power up

    // Clock divider table entry for mclk=4.096 MHz, Fs=16 kHz.
    es8311_w(0x02, 0x00);
    es8311_w(0x03, 0x10);   // adc_osr
    es8311_w(0x04, 0x10);   // dac_osr
    es8311_w(0x05, 0x00);   // adc/dac div
    es8311_w(0x06, 0x03);   // bclk div
    es8311_w(0x07, 0x00);   // lrck high
    es8311_w(0x08, 0xFF);   // lrck low

    // Serial digital interface: I2S, 16-bit, in + out.
    es8311_w(0x09, 0x0C);
    es8311_w(0x0A, 0x0C);

    // System power / analog bring-up.
    es8311_w(0x0B, 0x00);
    es8311_w(0x0C, 0x00);
    es8311_w(0x10, 0x1F);   // analog power / VMID fast charge
    es8311_w(0x11, 0x7F);   // charge pump / output drivers
    es8311_w(0x00, 0x80);   // re-assert power-up
    es8311_w(0x0D, 0x01);   // power up analog circuits
    es8311_w(0x0E, 0x02);   // ADC/PGA modulator (kept per reference)
    es8311_w(0x12, 0x00);   // power up DAC
    es8311_w(0x13, 0x10);   // enable output to HP/line drive  (critical)
    es8311_w(0x14, 0x1A);   // ADC select / PGA gain (mic; harmless for DAC)

    // DAC path: ramp, unmute, volume.
    es8311_w(0x37, 0x08);   // DAC ramp / bypass EQ
    es8311_w(0x31, 0x00);   // DAC UNMUTE (clear mute bits 5/6) — was muted
    es8311_w(0x32, 0xA0);   // DAC volume — moderate; fine-tune with amplitude below
    return true;
}

// ---- I2S TX channel ------------------------------------------------------
static i2s_chan_handle_t s_tx = nullptr;

static bool i2s_setup(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws   = (gpio_num_t)I2S_LRCK_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din  = (gpio_num_t)I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // ES8311 wants MCLK = 256 * Fs (default mclk_multiple is 256, set explicitly).
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) return false;
    // Keep the channel enabled for the device's lifetime. We feed it silence
    // between chimes (audio_hal_tick) so the DMA never underruns — an
    // underrunning channel replays its last buffer as a stuck tone, and
    // enable/disable cycling replays stale samples as phantom leading notes.
    return i2s_channel_enable(s_tx) == ESP_OK;
}

// ---- Retro theme: synthesized chimes -------------------------------------
// The "Retro" theme (index 0) keeps these synth chimes; the sampled themes use
// PCM from audio_samples.h instead. Each chime is a short list of
// (frequency, duration) notes. PERMISSION rises ("I need you"); ASK lilts
// upward like a question; DONE falls ("finished"). All three are distinct so
// permission / plan / done are tellable apart by ear even on Retro.
struct chime_note_t { uint16_t hz; uint16_t ms; };

static const chime_note_t CHIME_PERMISSION[] = { {523, 90}, {659, 90}, {784, 140} }; // C5 E5 G5 ↑
static const chime_note_t CHIME_ASK[]        = { {659, 90}, {988, 170} };            // E5 B5 ↗ (question)
static const chime_note_t CHIME_DONE[]       = { {784, 110}, {523, 160} };           // G5 C5 ↓
#define N_NOTES(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

// Playback state machine, advanced from audio_hal_tick(). Synth and PCM paths
// are mutually exclusive (s_seq XOR s_pcm). Synth renders one note per tick;
// PCM streams one chunk per tick. Either way the main loop never stalls long.
static const chime_note_t* s_seq   = nullptr;
static uint8_t        s_seq_len = 0;
static uint8_t        s_idx     = 0;
static double         s_phase   = 0.0;   // carried across tick boundaries (continuous)

// Sampled-theme playback: a pointer straight into the flash PCM array (no RAM
// copy — safe on the no-PSRAM C6) plus a cursor.
static const int16_t* s_pcm     = nullptr;
static uint32_t       s_pcm_len = 0;
static uint32_t       s_pcm_pos = 0;

static uint8_t s_theme = 0;   // 0 = Retro synth; >0 = sampled theme in SND_TABLE

// Volume: a continuous 0..255 value scales the sine amplitude linearly up to
// VOL_AMP_MAX. 0 = silent (audio_hal_play early-returns). The DAC hardware gain
// (reg 0x32) is fixed; we scale the PCM waveform instead so there's no I2C
// latency mid-chime. VOL_AMP_MAX is the loudest amplitude (was the old level-3).
#define VOL_AMP_MAX 18000.0f
static uint8_t s_vol = 160;          // 0..255 (raw); ~mid by default
static float   s_amp = VOL_AMP_MAX * 160.0f / 255.0f;

static void render_note(const chime_note_t& n) {
    const int total = (int)((long)SAMPLE_RATE * n.ms / 1000);
    const double dphi = 2.0 * M_PI * n.hz / SAMPLE_RATE;
    // Linear attack/release envelope (~6 ms each) to kill clicks.
    const int edge = SAMPLE_RATE * 6 / 1000;

    static int16_t buf[256];
    int done = 0;
    while (done < total) {
        int chunk = total - done;
        if (chunk > (int)(sizeof(buf) / sizeof(buf[0]))) chunk = sizeof(buf) / sizeof(buf[0]);
        for (int i = 0; i < chunk; i++) {
            int g = done + i;
            float env = 1.0f;
            if (g < edge)            env = (float)g / edge;
            else if (g > total - edge) env = (float)(total - g) / edge;
            buf[i] = (int16_t)(sin(s_phase) * s_amp * env);
            s_phase += dphi;
            if (s_phase > 2.0 * M_PI) s_phase -= 2.0 * M_PI;
        }
        size_t wrote = 0;
        i2s_channel_write(s_tx, buf, chunk * sizeof(int16_t), &wrote, portMAX_DELAY);
        done += chunk;
    }
}

// ---- HAL -----------------------------------------------------------------
static bool s_ready = false;

void audio_hal_init(void) {
    if (!es8311_init()) { Serial.println("ES8311 init FAILED"); return; }
    if (!i2s_setup())   { Serial.println("I2S init FAILED"); return; }
    s_ready = true;
    Serial.println("Audio ready (ES8311 + I2S, 16 kHz mono)");
}

void audio_hal_play(audio_sound_t sound) {
    if (!s_ready || s_seq || s_pcm) return;   // ignore if busy with a sound
    if (s_vol == 0) return;                   // muted

    // Sampled theme: play the PCM clip for this event if the active theme has
    // one. Falls through to the synth chime when the entry is empty (Retro, or
    // a theme that didn't bundle this event).
    if (s_theme < SND_THEME_COUNT && (int)sound < SND_EVENT_COUNT) {
        const sound_sample_t& s = SND_TABLE[s_theme][(int)sound];
        if (s.data && s.len) {
            s_pcm = s.data;
            s_pcm_len = s.len;
            s_pcm_pos = 0;
            return;
        }
    }

    // Retro / fallback: synthesized chime.
    switch (sound) {
        case SND_PERMISSION: s_seq = CHIME_PERMISSION; s_seq_len = N_NOTES(CHIME_PERMISSION); break;
        case SND_ASK:        s_seq = CHIME_ASK;        s_seq_len = N_NOTES(CHIME_ASK);        break;
        default:             s_seq = CHIME_DONE;       s_seq_len = N_NOTES(CHIME_DONE);       break;
    }
    s_idx = 0;
    s_phase = 0.0;
}

void audio_hal_set_theme(uint8_t idx) {
    s_theme = idx;
}

void audio_hal_set_volume(uint8_t val) {
    s_vol = val;
    s_amp = VOL_AMP_MAX * (float)val / 255.0f;
}

uint8_t audio_hal_get_volume(void) { return s_vol; }

void audio_hal_tick(void) {
    if (!s_ready) return;
    if (s_pcm) {
        // Stream one chunk straight from the flash PCM array, scaling each
        // sample by volume (samples are peak-normalized at build time to match
        // the synth's amplitude, so vol=255 is full level).
        static int16_t buf[256];
        uint32_t remain = s_pcm_len - s_pcm_pos;
        uint32_t chunk  = remain < 256 ? remain : 256;
        for (uint32_t i = 0; i < chunk; i++)
            buf[i] = (int16_t)((int32_t)s_pcm[s_pcm_pos + i] * s_vol / 255);
        size_t wrote = 0;
        i2s_channel_write(s_tx, buf, chunk * sizeof(int16_t), &wrote, portMAX_DELAY);
        s_pcm_pos += chunk;
        if (s_pcm_pos >= s_pcm_len) {
            s_pcm = nullptr;                 // clip done
            static const int16_t flush[512] = {0};
            size_t w = 0;
            for (int i = 0; i < 4; i++)
                i2s_channel_write(s_tx, flush, sizeof(flush), &w, pdMS_TO_TICKS(20));
        }
    } else if (s_seq) {
        render_note(s_seq[s_idx]);
        s_phase = 0.0;                   // restart phase per note (gap-free enough at these durations)
        if (++s_idx >= s_seq_len) {
            s_seq = nullptr;             // chime done
            // Immediately overwrite the whole DMA ring with silence so the
            // last note can't replay on the next underrun.
            static const int16_t flush[512] = {0};
            size_t w = 0;
            for (int i = 0; i < 4; i++)
                i2s_channel_write(s_tx, flush, sizeof(flush), &w, pdMS_TO_TICKS(20));
        }
    } else {
        // Idle: keep the always-on channel fed with silence so the DMA never
        // underruns (an underrun replays the last buffer as a stuck/looping
        // tone). At 16 kHz the loop drains ~80 samples/iteration; feed 256 with
        // a short timeout so we stay comfortably ahead without stalling.
        static const int16_t silence[256] = {0};
        size_t wrote = 0;
        i2s_channel_write(s_tx, silence, sizeof(silence), &wrote, pdMS_TO_TICKS(2));
    }
}
