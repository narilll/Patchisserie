// res/strudel-docs.json (scripts/build-docs.mjs): the editor's completions and hover docs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const file = `${root}res/strudel-docs.json`;
const docs = JSON.parse(readFileSync(file, 'utf8'));
const find = (name, source = 'strudel') =>
  docs.find((e) => e.source === source && (e.name === name || e.synonyms.includes(name)));

test('Strudel entries: description, examples, synonyms', () => {
  for (const name of ['lpf', 'note', 's', 'sound']) {
    const e = find(name);
    assert.ok(e, `${name} missing`);
    assert.ok(e.description.length > 10, `${name}: no description`);
    assert.ok(e.examples.length >= 1, `${name}: no example`);
  }
  assert.ok(['cutoff', 'ctf'].every((s) => find('lpf').synonyms.includes(s)));
  assert.equal(find('sound').name, 's');
  assert.equal(find('lpf').signature, 'lpf(frequency)');
});

test('Hydra entries: inputs with defaults; our globals', () => {
  const osc = find('osc', 'hydra');
  assert.deepEqual(
    osc.params.map((p) => [p.name, p.default]),
    [
      ['frequency', '60'],
      ['sync', '0.1'],
      ['offset', '0'],
    ],
  );
  assert.equal(osc.signature, 'osc(frequency = 60, sync = 0.1, offset = 0)');
  assert.equal(find('blend', 'hydra').signature, '.blend(texture, amount = 0.5)');
  for (const name of ['initHydra', 'loadScript', 'fft', 'setBins']) assert.ok(find(name, 'hydra'), name);
  assert.match(find('midin').description, /Filo's menu/); // ours, not strudel.cc's
});

test('plain text, sorted by name (Strudel first)', () => {
  const tag = /<\/?(p|a|code|pre|br|ul|ol|li|em|strong|h\d|div|span)\b[^>]*>/i;
  for (const e of docs) {
    assert.doesNotMatch(e.description, tag, e.name);
    assert.doesNotMatch(e.description, /&[a-z]+;|\{@link/, e.name);
    for (const p of e.params) assert.doesNotMatch(p.description ?? '', tag, `${e.name} ${p.name}`);
  }
  const keys = docs.map((e) => `${e.name} ${e.source === 'strudel' ? 0 : 1}`);
  assert.deepEqual(
    keys,
    [...keys].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)),
  );
});

// Needs the Strudel checkout and jsdoc of scripts/update-engine (offline: no install here).
const ready =
  existsSync(`${root}build/engine-src/strudel/pnpm-lock.yaml`) && existsSync(`${root}build/jsdoc/node_modules/jsdoc`);
test(
  'the generator is deterministic and the committed file is its output',
  { skip: !ready && 'no build/jsdoc' },
  () => {
    const tmp = mkdtempSync(join(tmpdir(), 'docs-'));
    try {
      const run = (out) => {
        execFileSync('node', [`${root}scripts/build-docs.mjs`, '--out', out], { stdio: 'ignore' });
        return readFileSync(out, 'utf8');
      };
      const a = run(join(tmp, 'a.json'));
      assert.equal(run(join(tmp, 'b.json')), a);
      assert.equal(readFileSync(file, 'utf8'), a, 'res/strudel-docs.json is stale: node scripts/build-docs.mjs');
    } finally {
      rmSync(tmp, { recursive: true, force: true });
    }
  },
);
