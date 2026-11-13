// The user manual (docs/) must stay in step with the plugin:
//  - plugin.json's manualUrls point at docs/README.md and docs/<module>.md, and those files exist;
//  - every panel label and menu label in src/plugin.cpp occurs verbatim in its module's manual;
//  - relative Markdown links in docs/*.md and README.md resolve; README.md links docs/README.md.
// ponytail: names, not prose — a behaviour change without a new label is caught by CLAUDE.md's rule, not here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const read = (p) => readFileSync(`${root}${p}`, 'utf8');
const plugin = JSON.parse(read('plugin.json'));
const BASE = 'https://github.com/narilll/Patchisserie/blob/main/docs/';

// module name -> { panel: [...], menu: [...] }, from the `struct XWidget : ModuleWidget {` regions of the source.
function extractLabels(src) {
  const out = {};
  const ends = [...src.matchAll(/^(?:struct |Model|namespace|static |void |\} \/\/)/gm)].map((m) => m.index);
  for (const m of src.matchAll(/^struct (\w+) : ModuleWidget \{/gm)) {
    const end = ends.find((e) => e > m.index) ?? src.length;
    const body = src.slice(m.index, end);
    const panel = [];
    for (const a of body.matchAll(/const char\* \w+\[\] = \{([^}]*)\}/g))
      for (const s of a[1].matchAll(/"([^"]*)"/g)) panel.push(s[1]);
    for (const b of body.matchAll(/\bband\("([^"]*)"/g)) panel.push(b[1]);
    for (const l of body.matchAll(/\blabel\(\s*\w+\s*,\s*"([^"]*)"/g)) panel.push(l[1]);
    const menu = [...body.matchAll(
      /create(?:MenuItem|BoolPtrMenuItem|IndexSubmenuItem|SubmenuItem|CheckMenuItem|MenuLabel|BoolMenuItem)\(\s*"([^"]*)"/g,
    )].map((x) => x[1].replace(/:? *$/, ''));
    out[m[1].replace(/Widget$/, '')] = { panel: panel.filter(Boolean), menu: menu.filter(Boolean) };
  }
  return out;
}

// -> list of failure messages for labels missing from doc text
function missingLabels(labels, docOf, docPath) {
  const fails = [];
  for (const [mod, { panel, menu }] of Object.entries(labels)) {
    const doc = docOf(mod);
    for (const [kind, list] of [['panel', panel], ['menu', menu]])
      for (const l of list)
        if (!doc.includes(l))
          fails.push(`${mod}: ${kind} label "${l}" is not in ${docPath(mod)}: document it`);
  }
  return fails;
}

const docPath = (mod) => `docs/${mod.toLowerCase()}.md`;

test('plugin.json manualUrls point at the manual', () => {
  assert.equal(plugin.manualUrl, `${BASE}README.md`);
  for (const m of plugin.modules) {
    assert.equal(m.manualUrl, `${BASE}${m.slug.toLowerCase()}.md`, `${m.slug} manualUrl`);
    assert.ok(existsSync(`${root}${docPath(m.slug)}`), `${docPath(m.slug)} is missing`);
  }
});

test('every panel and menu label is in its module manual', () => {
  const labels = extractLabels(read('src/plugin.cpp'));
  for (const m of plugin.modules) {
    assert.ok(labels[m.slug]?.panel.length, `no panel labels extracted for ${m.slug}`);
  }
  assert.ok(labels.Icing.panel.includes('AUDIO'), 'Icing AUDIO label not extracted');
  const docOf = (mod) => (existsSync(`${root}${docPath(mod)}`) ? read(docPath(mod)) : '');
  assert.deepEqual(missingLabels(labels, docOf, docPath), []);
});

test('the checker reports a label missing from the manual (fixture)', () => {
  const src = 'struct FakeWidget : ModuleWidget {\n  menu->addChild(createMenuItem("Secret option", "", f));\n};\n\nstatic int x;\n';
  const labels = extractLabels(src);
  assert.deepEqual(labels, { Fake: { panel: [], menu: ['Secret option'] } });
  const fails = missingLabels(labels, () => 'nothing here', docPath);
  assert.equal(fails.length, 1);
  assert.match(fails[0], /Fake: menu label "Secret option" is not in docs\/fake\.md/);
  assert.deepEqual(missingLabels(labels, () => 'the Secret option is documented', docPath), []);
});

test('relative Markdown links resolve; README.md links the manual', () => {
  const files = ['README.md', ...readdirSync(`${root}docs`).filter((f) => f.endsWith('.md')).map((f) => `docs/${f}`)];
  for (const f of files)
    for (const m of read(f).matchAll(/\[[^\]]*\]\(([^)\s]+)\)/g)) {
      const target = m[1].split('#')[0];
      if (!target || /^(https?:|mailto:)/.test(m[1])) continue;
      assert.ok(existsSync(resolve(root, dirname(f), target)), `${f}: broken link ${m[1]}`);
    }
  assert.ok(read('README.md').includes('](docs/README.md)'), 'README.md must link docs/README.md');
});
