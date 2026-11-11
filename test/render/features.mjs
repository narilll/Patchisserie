// Deterministic audio features for the render regression, shared by references (scripts/render-reference.mjs)
// and our renders (test/render.test.mjs). Node stdlib only.

// RIFF WAVE: PCM 16/24/32-bit or IEEE float 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE. Returns channels as Float64Array.
export function readWav(buf) {
  let fmt, data, size;
  for (let o = 12; o + 8 <= buf.length; o += 8 + size + (size & 1)) {
    size = buf.readUInt32LE(o + 4);
    const id = buf.toString('ascii', o, o + 4);
    if (id === 'fmt ') fmt = o + 8;
    if (id === 'data') data = [o + 8, Math.min(size, buf.length - o - 8)];
  }
  if (buf.toString('ascii', 0, 4) !== 'RIFF' || !fmt || !data) throw new Error('not a RIFF WAVE file');
  const format = buf.readUInt16LE(fmt) === 0xfffe ? buf.readUInt16LE(fmt + 24) : buf.readUInt16LE(fmt);
  const channels = buf.readUInt16LE(fmt + 2);
  const sampleRate = buf.readUInt32LE(fmt + 4);
  const bits = buf.readUInt16LE(fmt + 14);
  const read = {
    '1:16': (o) => buf.readInt16LE(o) / 0x8000,
    '1:24': (o) => buf.readIntLE(o, 3) / 0x800000,
    '1:32': (o) => buf.readInt32LE(o) / 0x80000000,
    '3:32': (o) => buf.readFloatLE(o),
    '3:64': (o) => buf.readDoubleLE(o),
  }[`${format}:${bits}`];
  if (!read) throw new Error(`unsupported WAV format ${format}, ${bits} bit`);
  const step = (bits / 8) * channels;
  const frames = Math.floor(data[1] / step);
  return {
    sampleRate,
    channels: Array.from({ length: channels }, (_, c) =>
      Float64Array.from({ length: frames }, (_, i) => read(data[0] + i * step + (c * bits) / 8)),
    ),
  };
}

// Zwicker critical band edges (Hz): 24 Bark bands, 0 Hz to 15.5 kHz.
export const BARK_EDGES = [
  0, 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400,
  7700, 9500, 12000, 15500,
];

const db = (power) => Math.round(100 * 10 * Math.log10(power + 1e-20)) / 100; // 0.01 dB, floor -200 dB

// In-place iterative radix-2 complex FFT; re.length is a power of two.
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) [re[i], re[j], im[i], im[j]] = [re[j], re[i], im[j], im[i]];
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < len / 2; k++) {
        const [c, s] = [Math.cos(ang * k), Math.sin(ang * k)];
        const [a, b] = [i + k, i + k + len / 2];
        const [tr, ti] = [re[b] * c - im[b] * s, re[b] * s + im[b] * c];
        [re[b], im[b], re[a], im[a]] = [re[a] - tr, im[a] - ti, re[a] + tr, im[a] + ti];
      }
    }
  }
}

// Mean power per sample of x in each Bark band, in dB: one rectangular-window FFT over the whole segment
// (zero-padded to a power of two), |X[k]|^2 summed per band (DC bin excluded). By Parseval the bands add up to the
// segment's mean square below 15.5 kHz. ponytail: no framing/window; a single long FFT is shift-invariant, which
// suits comparing two renders of the same pattern.
export function barkDb(x, sampleRate) {
  let n = 1;
  while (n < x.length) n <<= 1;
  const re = new Float64Array(n),
    im = new Float64Array(n);
  re.set(x);
  fft(re, im);
  const bands = new Float64Array(BARK_EDGES.length - 1);
  for (let k = 1, b = 0; k <= n / 2 && b < bands.length; k++) {
    const f = (k * sampleRate) / n;
    while (b < bands.length && f >= BARK_EDGES[b + 1]) b++;
    if (b < bands.length) bands[b] += ((k === n / 2 ? 1 : 2) * (re[k] ** 2 + im[k] ** 2)) / n / x.length;
  }
  return Array.from(bands, db);
}

// Onsets: frames where the channel-mean signal starts a new event out of (near) silence. Envelope e[k] = max|x| over
// hop k (HOP samples, hop grid aligned to frame 0). Hop k is an onset hop when e[k] >= GATE * (file peak) and
// e[k] >= RISE * max(e[k-LOOKBACK..k-1]). The onset frame is the first sample from hop k-1 on whose |x| exceeds
// REFINE * (peak of the REFINE_SPAN samples from hop k-1 on); RISE >> 1/REFINE keeps hop k-1's residue below that, so
// the frame lands on the attack itself. Integer frames, no smoothing: deterministic, sample-exact for equal waveforms.
// ponytail: tuned for test/render/patterns.json. On this set events are either >= +42 dB over the preceding 4 hops
// (out of silence) or <= +20 dB (legato note changes, delay echoes, hits over tails), so RISE = 30 dB sits >= 10 dB from
// every event; those masked events are covered by the per-cycle Bark bands instead. A spectral-flux detector is the
// upgrade path if overlapping onsets must be timed.
const [HOP, LOOKBACK, RISE, GATE, REFINE, REFINE_SPAN] = [64, 4, 10 ** (30 / 20), 10 ** (-40 / 20), 0.1, 1024];
export function onsets(x) {
  const hops = Math.floor(x.length / HOP);
  const e = new Float64Array(hops);
  for (let k = 0; k < hops; k++) for (let i = k * HOP; i < (k + 1) * HOP; i++) e[k] = Math.max(e[k], Math.abs(x[i]));
  const gate = GATE * e.reduce((a, b) => Math.max(a, b), 0);
  const found = [];
  for (let k = 0; k < hops; k++) {
    let before = 0;
    for (let j = Math.max(0, k - LOOKBACK); j < k; j++) before = Math.max(before, e[j]);
    if (e[k] < gate || e[k] < RISE * before) continue;
    const start = Math.max(0, (k - 1) * HOP);
    let peak = 0;
    for (let i = start; i < Math.min(x.length, start + REFINE_SPAN); i++) peak = Math.max(peak, Math.abs(x[i]));
    let i = start;
    while (Math.abs(x[i]) <= REFINE * peak) i++;
    if (i !== found.at(-1)) found.push(i); // a slow attack can pass in two consecutive hops and refine to one frame
  }
  return found;
}

// All features of a decoded WAV: channel mean (L+R)/2 analysed as a whole and per cycle (sampleRate / cps frames).
export function features({ sampleRate, channels }, cps) {
  const x = channels[0].map((_, i) => channels.reduce((s, c) => s + c[i], 0) / channels.length);
  const cycle = Math.round(sampleRate / cps);
  const cycles = Array.from({ length: Math.floor(x.length / cycle) }, (_, c) => x.subarray(c * cycle, (c + 1) * cycle));
  return {
    frames: x.length,
    rms_db: db(x.reduce((s, v) => s + v * v, 0) / x.length),
    onsets: onsets(x),
    bark_db: barkDb(x, sampleRate),
    bark_db_cycles: cycles.map((c) => barkDb(c, sampleRate)),
  };
}
