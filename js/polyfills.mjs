// Imported first by host.mjs, so it runs before any Strudel module body.
// QuickJS's console only has log (Strudel calls warn/error/info at import and eval time).
// ponytail: all levels print alike; the C++ engine (M2) installs a leveled console that feeds the editor log.
for (const level of ['warn', 'error', 'info', 'debug']) console[level] ??= console.log;
