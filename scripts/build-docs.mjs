// res/strudel-docs.json, the editor's completions and hover docs, from the Strudel checkout of scripts/update-engine:
// Strudel's doc.json (its `jsdoc-json` script, with the jsdoc + jsdoc-json versions its lockfile pins, installed into
// build/jsdoc), Hydra's functions (hydra-synth's glsl-functions.js) and our own globals (scripts/docs-extra.json).
// Usage: node scripts/build-docs.mjs [--out <file>] (default res/strudel-docs.json)
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { relative } from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const src = `${root}build/engine-src/strudel`;
const outArg = process.argv.indexOf('--out');
const out = outArg > 0 ? process.argv[outArg + 1] : `${root}res/strudel-docs.json`;

// --- jsdoc at Strudel's lockfile versions, its dependencies too (npm overrides) ---

const lock = readFileSync(`${src}/pnpm-lock.yaml`, 'utf8');
const unquote = (s) => s.replace(/^'|'$/g, '');
// snapshots: `  name@version(peers):` then `    dependencies:` / `    optionalDependencies:` and `      name: version`.
const snapshots = new Map();
let current;
for (const line of lock.slice(lock.indexOf('\nsnapshots:\n')).split('\n')) {
  let m;
  if ((m = line.match(/^ {2}(\S.*?):( \{\})?$/))) snapshots.set(unquote(m[1]), (current = {}));
  else if (current && (m = line.match(/^ {6}(\S+): (\S+)$/))) current[unquote(m[1])] = m[2];
}
const importer = lock.slice(lock.indexOf('\n  .:\n'), lock.indexOf('\n\n', lock.indexOf('\n  .:\n') + 1));
const rootVersion = (name) =>
  importer.match(new RegExp(`\\n {6}${name}:\\n {8}specifier: .*\\n {8}version: (\\S+)`))[1];
const tools = { jsdoc: rootVersion('jsdoc'), 'jsdoc-json': rootVersion('jsdoc-json') };
const pins = {};
const walk = (name, version) => {
  const plain = version.replace(/\(.*$/, '');
  if (pins[name] === plain) return;
  if (pins[name]) throw new Error(`build-docs: ${name} at ${pins[name]} and ${plain} in jsdoc's tree`); // ponytail: flat pins
  pins[name] = plain;
  for (const [dep, v] of Object.entries(snapshots.get(`${name}@${version}`) ?? {})) walk(dep, v);
};
for (const [name, version] of Object.entries(tools)) walk(name, version);
const overrides = Object.fromEntries(Object.entries(pins).filter(([name]) => !(name in tools)));
const manifest = JSON.stringify({ private: true, dependencies: tools, overrides }, null, 2) + '\n';
const jsdocDir = `${root}build/jsdoc`;
const manifestFile = `${jsdocDir}/package.json`;
if (!existsSync(`${jsdocDir}/node_modules/jsdoc`) || readFileSync(manifestFile, 'utf8') !== manifest) {
  mkdirSync(jsdocDir, { recursive: true });
  writeFileSync(manifestFile, manifest);
  execFileSync('npm', ['install', '--ignore-scripts', '--no-audit', '--no-fund', '--no-package-lock'], {
    cwd: jsdocDir,
    stdio: 'inherit',
  });
}

// Strudel's `jsdoc-json` script, into build/.
const rawFile = `${root}build/strudel-doc.json`;
execFileSync(
  'node',
  [
    `${jsdocDir}/node_modules/jsdoc/jsdoc.js`,
    'packages/',
    '--template',
    `${jsdocDir}/node_modules/jsdoc-json`,
    '--destination',
    rawFile,
    '-c',
    'jsdoc/jsdoc.config.json',
  ],
  { cwd: src, stdio: ['ignore', 'ignore', 'inherit'] },
);

// --- Slim: what strudel.cc's Reference panel lists, as plain text ---

const entities = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' };
const decode = (s) =>
  s.replace(/&(#x[0-9a-f]+|#\d+|\w+);/gi, (m, e) =>
    e[0] !== '#'
      ? (entities[e] ?? m)
      : String.fromCodePoint(parseInt(e.slice(e[1] === 'x' ? 2 : 1), e[1] === 'x' ? 16 : 10)),
  );
const inline = (html) =>
  html
    .replace(/\{@link\s+([^}|\s]+)\s*\|?\s*([^}]*)\}/g, (_, target, label) => `\`${label || target}\``)
    .replace(/\s+/g, ' ')
    .replace(/<\/?code>/g, '`')
    .replace(/<br\s*\/?>/g, '\n')
    .replace(/<li>/g, '\n- ')
    .replace(/<\/(p|ul|ol|h\d|div|blockquote)>/g, '\n\n')
    .replace(/<[^>]*>/g, '');
/** jsdoc's markdown HTML as text: paragraphs, <br> and list items as line breaks, <code> and <pre> blocks (their lines
kept) as `code`, no other tags. */
const plain = (html = '') =>
  decode(
    html
      .split(/(<pre[^>]*>[\s\S]*?<\/pre>)/)
      .map((part, i) => (i % 2 ? `\n\n\`${part.replace(/<[^>]*>/g, '').replace(/\n+$/, '')}\`\n\n` : inline(part)))
      .join(''),
  )
    .replace(/ *\n */g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
const signature = (name, params, method = false) =>
  `${method ? '.' : ''}${name}(${params
    .filter((p) => !p.name.includes('.')) // options.x: a member of the param before
    .map((p) => (p.default !== undefined ? `${p.name} = ${p.default}` : p.name))
    .join(', ')})`;

const jsdoc = JSON.parse(readFileSync(rawFile, 'utf8')).docs;
// Reference.jsx's isValid plus autocomplete.mjs's excluded tags. `tags`: @tags values (strings) or jsdoc's unknown tags.
const tagged = (d, t) => (d.tags ?? []).some((x) => x === t || x?.originalTitle === t);
const valid = (d) =>
  d.name &&
  !d.name.startsWith('_') &&
  d.description &&
  d.kind !== 'package' &&
  !(tagged(d, 'superdirt') && !tagged(d, 'superdough')) &&
  !tagged(d, 'superdirtOnly') &&
  !tagged(d, 'noAutocomplete');
// jsdoc's order is the file system's: by file and line instead, so the first of a name (Reference keeps it) is stable.
const where = (d) => `${relative(src, d.meta?.path ?? '')}/${d.meta?.filename ?? ''}`;
const ordered = jsdoc
  .filter(valid)
  .sort((a, b) => (where(a) < where(b) ? -1 : where(a) > where(b) ? 1 : (a.meta?.lineno ?? 0) - (b.meta?.lineno ?? 0)));
const seen = new Set();
const strudel = [];
for (const d of ordered) {
  if (seen.has(d.name)) continue;
  seen.add(d.name);
  const synonyms = (d.synonyms ?? []).filter((s) => s && !seen.has(s));
  synonyms.forEach((s) => seen.add(s));
  const params = (d.params ?? []).map((p) => ({
    name: p.name ?? '',
    type: p.type?.names?.join(' | ') ?? '',
    description: plain(p.description),
  }));
  strudel.push({
    name: d.name,
    synonyms,
    signature: signature(d.name, params),
    params,
    description: plain(d.description),
    examples: d.examples ?? [],
    source: 'strudel',
  });
}

// Hydra: hydra-synth's functions, as hydra-synth generates them (sources global, the rest methods; a blend or modulate
// takes the other source first).
const hydraKinds = {
  src: 'Hydra source',
  coord: 'Hydra geometry transform',
  color: 'Hydra color transform',
  combine: 'Hydra blend: combines with the texture (a source)',
  combineCoord: 'Hydra modulation: the texture (a source) shifts the coordinates',
};
const { default: glsl } = await import(`${src}/packages/hydra/node_modules/hydra-synth/src/glsl/glsl-functions.js`);
const hydra = glsl().map((f) => {
  const params = [
    ...(f.type.startsWith('combine') ? [{ name: 'texture', type: 'vec4' }] : []),
    ...f.inputs.map((i) => ({
      name: i.name,
      type: i.type,
      ...(typeof i.default === 'number' && Number.isFinite(i.default) ? { default: String(i.default) } : {}),
    })),
  ];
  return {
    name: f.name,
    synonyms: [],
    signature: signature(f.name, params, f.type !== 'src'),
    params,
    description: hydraKinds[f.type] ?? `Hydra ${f.type}`,
    examples: [],
    source: 'hydra',
  };
});

// Ours replace Strudel's entry of the same name (strudel.cc's differ here).
const extra = JSON.parse(readFileSync(`${root}scripts/docs-extra.json`, 'utf8')).map((e) => ({
  synonyms: [],
  params: [],
  examples: [],
  ...e,
}));
const ours = new Set(extra.map((e) => e.name));
const keys = ['name', 'synonyms', 'signature', 'params', 'description', 'examples', 'source'];
const entries = [...strudel.filter((e) => !ours.has(e.name)), ...hydra, ...extra]
  .map((e) => Object.fromEntries(keys.map((k) => [k, e[k]])))
  .sort((a, b) =>
    a.name < b.name ? -1 : a.name > b.name ? 1 : a.source < b.source ? 1 : a.source > b.source ? -1 : 0,
  );
// One entry per line: diffs stay readable.
writeFileSync(out, `[\n${entries.map((e) => JSON.stringify(e)).join(',\n')}\n]\n`);
const count = (s) => entries.filter((e) => e.source === s).length;
console.log(`build-docs: ${out}: ${entries.length} entries (${count('strudel')} strudel, ${count('hydra')} hydra)`);
