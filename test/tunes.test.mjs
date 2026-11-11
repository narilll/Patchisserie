// Upstream tune snapshots reproduced through our engine in Node; test/qjs_tunes.mjs does the same under QuickJS.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { setup, notComparable } from './tunes.mjs';

const read = (path) => readFileSync(new URL(path, import.meta.url), 'utf8');
vm.runInThisContext(read('../res/engine.js'), { filename: 'res/engine.js' });
const tunes = await setup(read);

for (const name of tunes.names) {
  test(`tune: ${name}`, async () => assert.equal(await tunes.actual(name), tunes.expected(name)));
}
for (const [name, reason] of Object.entries(notComparable)) test(`tune: ${name}`, { skip: reason }, () => {});
