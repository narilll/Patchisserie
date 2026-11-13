// res/editor-themes.json, Filo's editor themes: the ones strudel.cc offers, from the Strudel checkout of
// scripts/update-engine. Strudel's themes.mjs is bundled with stubs for CodeMirror and Lezer (the theme files run
// unmodified) and evaluated, since some colours are computed; each theme is then mapped to Filo's colour roles.
// Usage: node scripts/build-themes.mjs [--out <file>] (default res/editor-themes.json)
import { build } from 'esbuild';
import { readFileSync, writeFileSync } from 'node:fs';

const root = new URL('..', import.meta.url).pathname;
const src = `${root}build/engine-src/strudel`;
const outArg = process.argv.indexOf('--out');
const out = outArg > 0 ? process.argv[outArg + 1] : `${root}res/editor-themes.json`;

// --- evaluate Strudel's themes with stubbed editor libraries ---

// A Lezer tag is a key ("keyword", "function(propertyName)"); `t.function` and the like are modifiers: callable.
const stubs = {
  name: 'themes-stubs',
  setup(b) {
    b.onResolve({ filter: /^@(codemirror\/.*|strudel\/draw)$/ }, (a) => ({ path: a.path, namespace: 'stub' }));
    b.onResolve({ filter: /^@lezer\/highlight$/ }, (a) => ({ path: a.path, namespace: 'stub' }));
    b.onResolve({ filter: /theme-helper\.mjs$/ }, () => ({ path: 'theme-helper', namespace: 'stub' }));
    b.onLoad({ filter: /^@(codemirror|strudel)\//, namespace: 'stub' }, () => ({
      contents: 'export const EditorView = {}, HighlightStyle = {}, syntaxHighlighting = {}, setTheme = {};',
    }));
    b.onLoad({ filter: /^@lezer\/highlight$/, namespace: 'stub' }, () => ({
      contents: `export const tags = new Proxy({}, { get: (_, name) => {
        const tag = (inner) => ({ key: name + '(' + inner.key + ')' });
        tag.key = name;
        return tag;
      } });`,
    }));
    b.onLoad({ filter: /^theme-helper$/, namespace: 'stub' }, () => ({
      contents: 'export const createTheme = (args) => args;',
    }));
  },
};
const bundle = await build({
  absWorkingDir: root,
  entryPoints: [`${src}/packages/codemirror/themes.mjs`],
  bundle: true,
  format: 'esm',
  write: false,
  logLevel: 'warning',
  plugins: [stubs],
});
const { themes, settings: exported } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);

// --- colours: #rrggbbaa ---

// ponytail: the named colours the themes use today; a new one fails the build, add it here.
const named = { black: '#000000', white: '#ffffff', red: '#ff0000', grey: '#808080', green: '#008000', blue: '#0000ff' };
const hex2 = (n) => Math.round(Math.min(255, Math.max(0, n))).toString(16).padStart(2, '0');
const rgba = (r, g, b, a = 1) => '#' + [r, g, b].map(hex2).join('') + hex2(a * 255);
function colour(v) {
  if (typeof v !== 'string') return null;
  const s = v.trim().toLowerCase();
  let m;
  if (s === 'transparent') return '#00000000';
  if (s in named) return named[s] + 'ff';
  if ((m = s.match(/^#([0-9a-f]{3,4})$/))) return '#' + [...m[1]].map((c) => c + c).join('') + (m[1].length === 3 ? 'ff' : '');
  if ((m = s.match(/^#([0-9a-f]{6})$/))) return `#${m[1]}ff`;
  if ((m = s.match(/^#([0-9a-f]{8})$/))) return `#${m[1]}`;
  if ((m = s.match(/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)$/)))
    return rgba(+m[1], +m[2], +m[3], m[4] === undefined ? 1 : +m[4]);
  if ((m = s.match(/^hsla?\(\s*([\d.]+)\s*,\s*([\d.]+)%\s*,\s*([\d.]+)%\s*(?:,\s*([\d.]+)\s*)?\)$/))) {
    const [h, sat, l] = [+m[1], m[2] / 100, m[3] / 100];
    const k = (n) => l - sat * Math.min(l, 1 - l) * Math.max(-1, Math.min((n + h / 30) % 12 - 3, 9 - ((n + h / 30) % 12), 1));
    return rgba(k(0) * 255, k(8) * 255, k(4) * 255, m[4] === undefined ? 1 : +m[4]);
  }
  return null;
}
const withAlpha = (c, k) => c.slice(0, 7) + hex2(parseInt(c.slice(7), 16) * k);

// --- Strudel theme -> Filo roles ---

const fallbacks = []; // "theme: role (from what)", printed
function roles(name) {
  const t = themes[name];
  const inline = t.settings ?? {};
  const get = (field, dflt) => {
    const raw = inline[field] ?? exported[name]?.[field] ?? dflt;
    if (raw === undefined) return undefined;
    const c = colour(raw);
    if (!c) throw new Error(`build-themes: ${name}: ${field}: cannot resolve ${JSON.stringify(raw)}`);
    return c;
  };
  const need = (field, v) => {
    if (v === undefined) throw new Error(`build-themes: ${name}: ${field} is missing`);
    return v;
  };
  const background = need('background', get('background', '#222'));
  const text = need('foreground', get('foreground'));
  // Lezer's tagHighlighter: for one tag the last style wins.
  const style = new Map();
  for (const s of t.styles)
    for (const tag of [s.tag].flat())
      if (s.color !== undefined) style.set(tag.key, s.color);
  // Only the tags Filo reads are resolved (upstream has typos in others, e.g. gruvboxDark's annotation).
  const tag = (...keys) => {
    const k = keys.find((k) => style.has(k));
    if (!k) return undefined;
    const raw = style.get(k);
    const c = raw === 'inherit' ? text : colour(raw); // CSS inherit: the editor's foreground
    if (!c) throw new Error(`build-themes: ${name}: style ${k}: cannot resolve ${JSON.stringify(raw)}`);
    return c;
  };

  const guttersTransparent = get('gutterBackground') === undefined || get('gutterBackground').endsWith('00');
  const call = tag('function(propertyName)', 'propertyName', 'function(variableName)', 'variableName');
  const label = tag('labelName');
  const operator = tag('operator');
  // An unstyled tag shows in the editor's foreground, as in CodeMirror.
  const [comment, string, number, keyword] = ['comment', 'string', 'number', 'keyword'].map((k) => {
    if (!tag(k)) fallbacks.push(`${name}: ${k} (text)`);
    return tag(k) ?? text;
  });
  if (!call) fallbacks.push(`${name}: call (text)`);
  if (!label) fallbacks.push(`${name}: label (keyword)`);
  if (!operator) fallbacks.push(`${name}: operator (text)`);
  if (inline.caret === undefined && exported[name]?.caret === undefined) fallbacks.push(`${name}: caret (text)`);
  if (get('selection') === undefined) fallbacks.push(`${name}: selection (text at 30 %)`);
  if (guttersTransparent) fallbacks.push(`${name}: gutter (background)`);
  if (get('gutterForeground') === undefined) fallbacks.push(`${name}: dim (text at 50 %)`);
  return {
    background,
    text,
    caret: get('caret') ?? text,
    selection: get('selection') ?? withAlpha(text, 0.3),
    gutter: guttersTransparent ? background : get('gutterBackground'),
    dim: get('gutterForeground') ?? withAlpha(text, 0.5),
    playing: text, // as strudel.cc's highlight.mjs: outline: solid 2px var(--foreground)
    comment,
    string,
    number,
    keyword,
    label: label ?? keyword,
    operator: operator ?? text,
    call: call ?? text,
  };
}

const list = Object.keys(themes).map((name) => ({ name, colours: roles(name) }));
const commit = readFileSync(`${root}res/ENGINE_VERSIONS`, 'utf8').match(/^strudel (\w+)/m)[1];
writeFileSync(out, JSON.stringify({ strudel: commit, themes: list }, null, 1) + '\n');
console.log(`${out}: ${list.length} themes from Strudel ${commit.slice(0, 7)}`);
for (const f of fallbacks) console.log(`  fallback ${f}`);
