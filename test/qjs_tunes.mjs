// Upstream tune snapshots reproduced through res/engine.js under QuickJS: build/qjs test/qjs_tunes.mjs
import * as std from 'qjs:std';
import { setup, notComparable } from './tunes.mjs';

const dir = import.meta.url.replace(/^file:\/\//, '').replace(/[^/]*$/, '');
const read = (path) => std.loadFile(dir + path);
std.loadScript(dir + '../res/engine.js');
const tunes = await setup(read);

let failed = 0;
for (const name of tunes.names) {
  const actual = await tunes.actual(name);
  const expected = tunes.expected(name);
  if (actual === expected) continue;
  failed++;
  const a = actual.split('\n');
  const e = (expected ?? '').split('\n');
  const i = a.findIndex((line, i) => line !== e[i]);
  print(`FAIL tune: ${name}\n  line ${i}\n  actual:   ${a[i]}\n  expected: ${e[i]}`);
}
for (const [name, reason] of Object.entries(notComparable)) print(`skip tune: ${name} # ${reason}`);
print(`qjs tunes: ${tunes.names.length - failed}/${tunes.names.length} match upstream snapshots`);
if (failed) std.exit(1);
