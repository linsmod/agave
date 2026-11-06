// Separate compile and execute cost for a real page bundle, in one isolate.
// The bundle is webpack-style and probes `window`/`document` at top level; we
// give it a minimal stub so execution can proceed far enough to be meaningful.
// The number that matters is the ratio compile:execute, not absolute parity
// with the browser (which has real DOM bindings).
const fs = require('node:fs');
const vm = require('node:vm');

const src = fs.readFileSync(process.argv[2], 'utf8');
const sandbox = {
  window: {}, self: {}, globalThis: {}, document: {},
  navigator: { userAgent: '', language: 'zh-CN', languages: ['zh-CN'] },
  location: { href: 'https://chat.deepseek.com/', protocol: 'https:', hostname: 'chat.deepseek.com' },
  console, setTimeout, clearTimeout, setInterval, clearInterval,
  performance: { now: () => Date.now() },
  Promise, JSON, Math, Date, Object, Array, String, Number, Boolean, Symbol,
  Map, Set, WeakMap, WeakSet, Error, TypeError, RegExp, Function,
  Uint8Array, ArrayBuffer, TextEncoder, TextDecoder,
};
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.globalThis = sandbox;
vm.createContext(sandbox);

const t0 = process.hrtime.bigint();
const script = new vm.Script(src, { filename: 'vendor.js' });
const t1 = process.hrtime.bigint();
let execMs = -1;
try {
  script.runInContext(sandbox);
  const t2 = process.hrtime.bigint();
  execMs = Number(t2 - t1) / 1e6;
} catch (e) {
  const t2 = process.hrtime.bigint();
  execMs = Number(t2 - t1) / 1e6;
  console.log(`execute threw: ${String(e).slice(0, 120)}`);
}
console.log(`compile=${(Number(t1 - t0) / 1e6).toFixed(1)}ms execute=${execMs.toFixed(1)}ms bytes=${src.length}`);
