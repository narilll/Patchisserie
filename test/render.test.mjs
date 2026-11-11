// Audio regression, plan gates G1 (onsets) and G3 (Bark bands): our offline renders build/render/<id>.wav (make render)
// against Chrome renders of the same Strudel commit (test/render/reference.json from scripts/render-reference.mjs).
// A pattern without a render is skipped, never passed. RENDER_DIR overrides build/render.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { BARK_EDGES, features, readWav } from './render/features.mjs';

const root = new URL('..', import.meta.url).pathname;
const dir = process.env.RENDER_DIR ?? `${root}build/render`;
const set = JSON.parse(readFileSync(`${root}test/render/patterns.json`, 'utf8'));
const reference = JSON.parse(readFileSync(`${root}test/render/reference.json`, 'utf8'));
const engine = /^strudel (\S+)/m.exec(readFileSync(`${root}res/ENGINE_VERSIONS`, 'utf8'))[1];

// The onset detector's threshold crossing lands only 0.2-4 % above its threshold on this set (bd over the sd tail:
// 0.2 %), so a render that differs only in resampler (Chrome resamples the 44.1 kHz samples at decode) or envelope
// rounding moves it by one frame. Every hap here starts on a whole frame, so a start 2+ frames off fails.
const ONSET_FRAMES = 1;
const BARK_DB = 3;
const RMS_DB = 1.5;
const FLOOR_DB = -60; // a band is compared when either side is within 60 dB of the reference's loudest band
// Random by design (superdough calls Math.random), so a render is one realization of the pattern: onsets within the
// 1 ms default attack (where a random waveform first crosses the threshold is random), no per-cycle bands, whole-file
// bands summed in groups of `group` Bark bands with floor -30 dB, rms ±3 dB. Spreads are max-min over 8 reference renders.
const RANDOM_ONSET_FRAMES = 48;
const RANDOM = {
  // spread 1.5 dB per band, rms 0.6 dB. Chrome sets the IR asynchronously, so reverb only starts in cycle 2.
  tri_room: { group: 1, why: 'room IR is generated noise (reverbGen.mjs)' },
  // spread 6.8 dB per band, 1.6 dB per 8 bands, onsets 7-12 frames into each hit.
  noise_hpf: { group: 8, why: 'white noise buffer (noise.mjs)' },
  // spread 4.2 dB per band (9.9 dB per cycle), 1.5 dB per 8 bands, rms 1.4 dB, onset 128 or 139.
  supersaw: { group: 8, why: 'random voice phases (worklets.mjs)' },
};

const grouped = (db, g) =>
  Array.from(
    { length: db.length / g },
    (_, i) => 10 * Math.log10(db.slice(i * g, (i + 1) * g).reduce((s, v) => s + 10 ** (v / 10), 0)),
  );

function bandFailures(ours, ref, { group = 1, floor = FLOOR_DB, refOnly = false, what }) {
  const [o, r] = [grouped(ours, group), grouped(ref, group)];
  const top = Math.max(...r) + floor;
  return r.flatMap((v, b) =>
    // refOnly: ours is one random realization, so near-silent bands (+4 dB spread at -33 dB) are judged by the fixed reference alone
    (refOnly ? v : Math.max(v, o[b])) > top && !(Math.abs(o[b] - v) <= BARK_DB)
      ? [
          `${what} ${BARK_EDGES[b * group]}-${BARK_EDGES[(b + 1) * group]} Hz: ${o[b].toFixed(2)} dB, reference ${v.toFixed(2)} dB`,
        ]
      : [],
  );
}

for (const { id } of set.patterns) {
  const file = `${dir}/${id}.wav`;
  const random = RANDOM[id];
  test(
    `${id}${random ? ` (random: ${random.why})` : ''}`,
    { skip: !existsSync(file) && `not rendered: ${file}` },
    () => {
      assert.equal(
        reference.strudel,
        engine,
        'test/render/reference.json is stale: run node scripts/render-reference.mjs',
      );
      const ref = reference.patterns[id];
      const wav = readWav(readFileSync(file));
      assert.equal(wav.sampleRate, set.sampleRate, 'sample rate');
      const ours = features(wav, set.cps);
      assert.equal(ours.frames, ref.frames, 'length in frames');

      const failures = [];
      if (!(Math.abs(ours.rms_db - ref.rms_db) <= (random ? 3 : RMS_DB)))
        failures.push(`rms ${ours.rms_db} dB, reference ${ref.rms_db} dB`);
      const tolerance = random ? RANDOM_ONSET_FRAMES : ONSET_FRAMES;
      const near =
        ours.onsets.length === ref.onsets.length &&
        ref.onsets.every((f, i) => Math.abs(ours.onsets[i] - f) <= tolerance);
      if (!near)
        failures.push(`onsets ${JSON.stringify(ours.onsets)}, reference ${JSON.stringify(ref.onsets)} (±${tolerance})`);
      if (random) {
        failures.push(
          ...bandFailures(ours.bark_db, ref.bark_db, { group: random.group, floor: -30, refOnly: true, what: 'whole file' }),
        );
      } else {
        failures.push(...bandFailures(ours.bark_db, ref.bark_db, { what: 'whole file' }));
        ref.bark_db_cycles.forEach((c, i) =>
          failures.push(...bandFailures(ours.bark_db_cycles[i], c, { what: `cycle ${i}` })),
        );
      }
      assert.deepEqual(failures, []);
    },
  );
}
