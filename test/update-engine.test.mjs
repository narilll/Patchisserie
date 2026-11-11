// scripts/update-engine's guards (plan "Updaty"), offline: a local fake upstream (STRUDEL_REPO) and --dry-run in a
// throwaway copy of the script's root (scripts/update-engine, src/audio/worklets.cpp, res/ENGINE_VERSIONS).
import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const tmp = mkdtempSync(join(tmpdir(), 'update-engine-'));
after(() => rmSync(tmp, { recursive: true, force: true }));

const upstream = join(tmp, 'upstream');
mkdirSync(upstream);
const git = (...args) =>
  execFileSync('git', ['-C', upstream, '-c', 'user.name=t', '-c', 'user.email=t@t', ...args], { encoding: 'utf8' }).trim();
git('init', '-q');
function commit(files) {
  for (const [path, text] of Object.entries(files)) {
    mkdirSync(dirname(join(upstream, path)), { recursive: true });
    writeFileSync(join(upstream, path), text);
  }
  git('add', '-A');
  git('commit', '-qm', Object.keys(files).join(' '));
  return git('rev-parse', 'HEAD');
}
const listed = readFileSync(`${root}src/audio/worklets.cpp`, 'utf8').matchAll(/\{"([a-z0-9-]+)", ProcessorDef::/g);
const names = [...listed].map((m) => m[1]);
const worklets = (list) => list.map((n) => `registerProcessor('${n}', class extends AudioWorkletProcessor {});`).join('\n');

const base = commit({ 'packages/superdough/worklets.mjs': worklets(names), 'packages/core/index.mjs': '1' });
const coreOnly = commit({ 'packages/core/index.mjs': '2' });
const audio = commit({ 'packages/superdough/superdough.mjs': 'changed', 'packages/webaudio/x.mjs': 'x' });
const soundfonts = commit({ 'packages/soundfonts/fonts.mjs': 'y' });
const unknown = commit({ 'packages/superdough/worklets.mjs': worklets([...names, 'new-processor']) });

/** update-engine --dry-run <args> with res/ENGINE_VERSIONS pinned to `pinned`. */
function run(pinned, ...args) {
  const dir = mkdtempSync(join(tmp, 'root-'));
  for (const f of ['scripts/update-engine', 'src/audio/worklets.cpp']) {
    mkdirSync(dirname(join(dir, f)), { recursive: true });
    cpSync(`${root}${f}`, join(dir, f));
  }
  mkdirSync(join(dir, 'res'));
  writeFileSync(join(dir, 'res/ENGINE_VERSIONS'), `strudel ${pinned} 2026-01-01T00:00:00+00:00 fake (ref: main)\n`);
  const p = spawnSync(join(dir, 'scripts/update-engine'), ['--dry-run', ...args], {
    env: { ...process.env, STRUDEL_REPO: upstream },
    encoding: 'utf8',
  });
  return { status: p.status, out: p.stdout + p.stderr };
}

test('kProcessors lists exactly the processors of the bundled res/worklets.js', () => {
  const bundled = [...readFileSync(`${root}res/worklets.js`, 'utf8').matchAll(/registerProcessor\("([^"]+)"/g)];
  assert.deepEqual(names.toSorted(), bundled.map((m) => m[1]).toSorted());
});

test('an update that leaves the audio packages alone passes', () => {
  const { status, out } = run(base, coreOnly);
  assert.equal(status, 0, out);
  assert.doesNotMatch(out, /superdough-ack/);
});

test('a superdough or webaudio change stops the update with the diff', () => {
  const { status, out } = run(base, audio);
  assert.equal(status, 1, out);
  assert.match(out, /STOP, the audio packages changed/);
  assert.match(out, /packages\/superdough\/superdough\.mjs \| 1 \+/); // git diff --stat
  assert.match(out, /2 files changed/);
  assert.match(out, new RegExp(`--ack-superdough ${audio}`));
});

test('a soundfonts change stops the update', () => {
  const { status, out } = run(audio, soundfonts);
  assert.equal(status, 1, out);
  assert.match(out, /packages\/soundfonts\/fonts\.mjs/);
});

test('acknowledged by the new sha, the update proceeds and records it', () => {
  const { status, out } = run(base, '--ack-superdough', audio, audio);
  assert.equal(status, 0, out);
  assert.match(out, new RegExp(`records superdough-ack ${audio}`));
});

test('an acknowledgement of another sha does not count', () => {
  const { status, out } = run(base, '--ack-superdough', coreOnly, audio);
  assert.equal(status, 1, out);
});

test('an unknown processor stops the update, acknowledged or not', () => {
  const { status, out } = run(soundfonts, '--ack-superdough', unknown, unknown);
  assert.equal(status, 1, out);
  assert.match(out, /kProcessors[\s\S]*new-processor/);
});
