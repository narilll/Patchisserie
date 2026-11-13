// res/editor-themes.json (scripts/build-themes.mjs): strudel.cc's editor themes mapped to Filo's colour roles.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';

const root = new URL('..', import.meta.url).pathname;
const data = JSON.parse(readFileSync(`${root}res/editor-themes.json`, 'utf8'));
const byName = (n) => data.themes.find((t) => t.name === n).colours;
const ROLES = ['background', 'text', 'caret', 'selection', 'gutter', 'dim', 'playing', 'comment', 'string', 'number', 'keyword', 'label', 'operator', 'call'];

test('39 themes, strudelTheme first, every role a #rrggbbaa', () => {
  assert.equal(data.themes.length, 39);
  assert.equal(data.themes[0].name, 'strudelTheme');
  for (const { name, colours } of data.themes) {
    assert.deepEqual(Object.keys(colours).sort(), [...ROLES].sort(), name);
    for (const [role, c] of Object.entries(colours)) assert.match(c, /^#[0-9a-f]{8}$/, `${name}.${role}`);
  }
});

test('strudelTheme has the colours of strudel-theme.mjs', () => {
  const c = byName('strudelTheme');
  assert.equal(c.background, '#222222ff');
  assert.equal(c.text, '#ffffffff');
  assert.equal(c.caret, '#ffcc00ff');
  assert.equal(c.keyword, '#c792eaff');
  assert.equal(c.string, '#c3e88dff');
  assert.equal(c.comment, '#7d8799ff');
  assert.equal(c.selection, '#80cbc480'); // rgba(128, 203, 196, 0.5)
  assert.equal(c.gutter, c.background); // gutterBackground is transparent
  assert.equal(c.playing, c.text); // highlight.mjs: outline: solid 2px var(--foreground)
});

test('githubLight is light', () => {
  const c = byName('githubLight');
  assert.equal(c.background, '#ffffffff');
  assert.equal(c.text, '#24292eff');
});

test('the names are those of the Strudel checkout, when there is one', (t) => {
  const file = `${root}build/engine-src/strudel/packages/codemirror/themes.mjs`;
  if (!existsSync(file)) return t.skip('no Strudel checkout (CI)');
  const src = readFileSync(file, 'utf8');
  const index = src.match(/export const themes = \{([^}]*)\}/)[1].split(',').map((s) => s.trim()).filter(Boolean);
  assert.deepEqual(data.themes.map((x) => x.name), index);
});
