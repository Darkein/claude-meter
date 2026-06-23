#!/usr/bin/env node
// pcm_to_c.js — convert WAV clips into firmware/src/.../audio_samples.h
//
// Pure Node (no ffmpeg): decodes PCM/float WAV, downmixes to mono, resamples to
// 16 kHz (linear), trims to a max length, applies short fades, peak-normalizes
// to match the Retro synth amplitude, and emits int16 C arrays + the
// SND_TABLE[theme][event] lookup that audio.cpp consumes.
//
// Sources/paths live in tools/sounds.json. Run: `node tools/pcm_to_c.js`
// (or via tools/build_sounds.sh). Missing files become empty {nullptr,0}
// entries, which the firmware falls back to the synth chime for.

const fs = require("fs");
const path = require("path");

const TOOLS_DIR = __dirname;
const OUT = path.join(
  TOOLS_DIR,
  "..",
  "firmware",
  "src",
  "boards",
  "waveshare_amoled_216_c6",
  "audio_samples.h"
);

const TARGET_RATE = 16000;
const MAX_SECONDS = 1.2; // cap each clip so flash stays small
const PEAK = 18000; // matches VOL_AMP_MAX in audio.cpp
const FADE_MS = 6; // de-click attack/release

// ---- WAV decode -> Float32 mono samples at the file's own rate -------------
function decodeWav(buf) {
  if (buf.toString("ascii", 0, 4) !== "RIFF" || buf.toString("ascii", 8, 12) !== "WAVE")
    throw new Error("not a RIFF/WAVE file");

  let fmt = null;
  let dataOff = -1;
  let dataLen = 0;
  let off = 12;
  while (off + 8 <= buf.length) {
    const id = buf.toString("ascii", off, off + 4);
    const size = buf.readUInt32LE(off + 4);
    const body = off + 8;
    if (id === "fmt ") {
      fmt = {
        audioFormat: buf.readUInt16LE(body),
        channels: buf.readUInt16LE(body + 2),
        sampleRate: buf.readUInt32LE(body + 4),
        bits: buf.readUInt16LE(body + 14),
      };
    } else if (id === "data") {
      dataOff = body;
      dataLen = size;
    }
    off = body + size + (size & 1); // chunks are word-aligned
  }
  if (!fmt || dataOff < 0) throw new Error("missing fmt/data chunk");

  const { audioFormat, channels, bits, sampleRate } = fmt;
  const bytesPer = bits >> 3;
  const frameBytes = bytesPer * channels;
  const frames = Math.floor(dataLen / frameBytes);
  const mono = new Float32Array(frames);

  const readSample = (p) => {
    if (audioFormat === 3 && bits === 32) return buf.readFloatLE(p); // IEEE float
    if (bits === 16) return buf.readInt16LE(p) / 32768;
    if (bits === 24) {
      const v = buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16);
      return (v & 0x800000 ? v - 0x1000000 : v) / 8388608;
    }
    if (bits === 32) return buf.readInt32LE(p) / 2147483648;
    if (bits === 8) return (buf[p] - 128) / 128; // unsigned PCM
    throw new Error("unsupported bit depth " + bits);
  };

  for (let f = 0; f < frames; f++) {
    let acc = 0;
    const base = dataOff + f * frameBytes;
    for (let c = 0; c < channels; c++) acc += readSample(base + c * bytesPer);
    mono[f] = acc / channels;
  }
  return { samples: mono, rate: sampleRate };
}

// ---- linear resample to TARGET_RATE ----------------------------------------
function resample(samples, rate) {
  if (rate === TARGET_RATE) return Float32Array.from(samples);
  const ratio = TARGET_RATE / rate;
  const outLen = Math.floor(samples.length * ratio);
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const src = i / ratio;
    const i0 = Math.floor(src);
    const i1 = Math.min(i0 + 1, samples.length - 1);
    const frac = src - i0;
    out[i] = samples[i0] * (1 - frac) + samples[i1] * frac;
  }
  return out;
}

// ---- trim, fade, peak-normalize, quantize to int16 -------------------------
function finalize(samples) {
  const maxLen = Math.floor(MAX_SECONDS * TARGET_RATE);
  let s = samples.length > maxLen ? samples.subarray(0, maxLen) : samples;

  const fade = Math.floor((FADE_MS / 1000) * TARGET_RATE);
  let peak = 0;
  for (let i = 0; i < s.length; i++) peak = Math.max(peak, Math.abs(s[i]));
  if (peak === 0) peak = 1;
  const gain = PEAK / (peak * 32768); // normalize then scale to PEAK in int16 units

  const out = new Int16Array(s.length);
  for (let i = 0; i < s.length; i++) {
    let env = 1;
    if (i < fade) env = i / fade;
    else if (i > s.length - fade) env = (s.length - i) / fade;
    let v = Math.round(s[i] * 32768 * gain * env);
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = v;
  }
  return out;
}

function emitArray(sym, int16) {
  const parts = [`static const int16_t ${sym}[] = {`];
  let line = "    ";
  for (let i = 0; i < int16.length; i++) {
    line += int16[i] + ",";
    if (line.length > 100) {
      parts.push(line);
      line = "    ";
    }
  }
  if (line.trim()) parts.push(line);
  parts.push("};");
  return parts.join("\n");
}

// ---- main ------------------------------------------------------------------
const manifest = JSON.parse(fs.readFileSync(path.join(TOOLS_DIR, "sounds.json"), "utf8"));
const { themes, events, files } = manifest;

const arrays = [];
const table = []; // table[themeIdx][eventIdx] = symbol or null

themes.forEach((theme, ti) => {
  table[ti] = [];
  events.forEach((event, ei) => {
    const rel = files[theme] && files[theme][event];
    if (!rel) {
      table[ti][ei] = null;
      return;
    }
    const wavPath = path.join(TOOLS_DIR, rel);
    if (!fs.existsSync(wavPath)) {
      console.warn(`! missing ${rel} — ${theme}/${event} falls back to synth`);
      table[ti][ei] = null;
      return;
    }
    const { samples, rate } = decodeWav(fs.readFileSync(wavPath));
    const pcm = finalize(resample(samples, rate));
    const sym = `snd_${theme}_${event}`;
    arrays.push(emitArray(sym, pcm));
    table[ti][ei] = { sym, len: pcm.length };
    console.log(`✓ ${theme}/${event}: ${rel} -> ${pcm.length} samples (${(pcm.length / TARGET_RATE).toFixed(2)}s)`);
  });
});

const header = `#pragma once
#include <stdint.h>

// GENERATED by tools/pcm_to_c.js (run tools/build_sounds.sh) — do not hand-edit.
//
// Sampled notification themes for the C6, embedded in flash as 16 kHz /
// 16-bit signed / mono PCM (matches the I2S config in audio.cpp). Samples are
// peak-normalized to ~${PEAK} so vol=255 matches the Retro synth's loudness;
// audio.cpp scales by volume at playback.
//
// Theme order matches soundtheme.cpp: ${themes.join(", ")}. Event order matches
// audio_sound_t: ${events.join(", ")}. Empty {nullptr,0} entries fall back to
// the synth chime, so the firmware builds and runs even before samples exist.

#define SND_THEME_COUNT ${themes.length}
#define SND_EVENT_COUNT ${events.length}

struct sound_sample_t {
    const int16_t* data;
    uint32_t       len;   // number of int16 samples
};

// ==== sample arrays ====
${arrays.length ? arrays.join("\n\n") : "// (none yet — drop WAVs in tools/sounds/ and rerun)"}

// ==== lookup table [theme][event] ====
static const sound_sample_t SND_TABLE[SND_THEME_COUNT][SND_EVENT_COUNT] = {
${themes
  .map((theme, ti) => {
    const row = events
      .map((_, ei) => {
        const cell = table[ti][ei];
        return cell ? `{${cell.sym}, ${cell.len}}` : "{nullptr, 0}";
      })
      .join(", ");
    return `    { ${row} }, // ${theme}`;
  })
  .join("\n")}
};
`;

fs.writeFileSync(OUT, header);
console.log(`\nWrote ${OUT}`);
