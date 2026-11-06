/* P1-g microbench: where does the G[T[++P]] bytecode-VM pattern spend time?
 * Run with the engine directly (no browser):
 *   H:\ant-v16\build-zig\ant.exe E:\mybrowsers\agave\debug\bench_vm.js
 */
var N = 2000000;
var G = [0, 0, 0, 0, 0, 0, 0, 0];
var T = [0, 1, 2, 3, 4, 5, 6, 7];
var O = { k0: 1, k1: 2, k2: 3, k3: 4, k4: 5, k5: 6, k6: 7, k7: 8 };

function timeIt(name, iters, fn) {
  var t0 = Date.now();
  var r = fn();
  var dt = Date.now() - t0;
  console.log(name + ': ' + dt + ' ms  (' + (dt * 1000000 / iters).toFixed(1) + ' ns/op)  r=' + r);
}

timeIt('numeric-array-read', N, function () {
  var s = 0;
  for (var i = 0; i < N; i++) s += G[i & 7];
  return s;
});

timeIt('numeric-array-write', N, function () {
  for (var i = 0; i < N; i++) G[i & 7] = i;
  return G[0];
});

timeIt('string-key-read', N, function () {
  var s = 0;
  var keys = ['k0', 'k1', 'k2', 'k3', 'k4', 'k5', 'k6', 'k7'];
  for (var i = 0; i < N; i++) s += O[keys[i & 7]];
  return s;
});

timeIt('dynamic-string-key-read', N, function () {
  var s = 0;
  for (var i = 0; i < N; i++) s += O['k' + (i & 7)];
  return s;
});

timeIt('switch-dispatch-loop', N / 8, function () {
  var P = 0;
  var n = 0;
  for (var i = 0; i < N / 8; i++) {
    for (;;) {
      switch (T[++P & 7]) {
        case 0: G[P & 7] += 1; break;
        case 1: G[P & 7] = G[P & 7] + 2; break;
        case 2: G[P & 7] = G[P & 7] - 1; break;
        case 3: G[P & 7] += String.fromCharCode(65); break;
        case 4: G[P & 7] = G[P & 7] << 2; break;
        default: n++; break;
      }
      if (n > 0) { n = 0; break; }
    }
  }
  return G[0];
});

/* The real shape: a closure (`j`) that re-enters itself with a fresh G array,
 * like the QIMEI/腾讯 VM's m(...) call in the captured stack. */
timeIt('vm-self-recursion', 200000, function () {
  var calls = 0;
  function j(depth) {
    var g = [0, 0, 0, 0];
    var t = [0, 1, 0, 1];
    var p = 0;
    var acc = 0;
    while (calls < 200000) {
      calls++;
      switch (t[++p & 3]) {
        case 0: g[p & 3] += 1; break;
        default: acc += depth < 20 ? j(depth + 1) : 1; break;
      }
      if (acc > 1000000) return g[0];
    }
    return acc + g[0];
  }
  return j(0);
});

/* Sparse arrays: the real VM's G slots come from a bytecode table, so writes
 * land on arbitrary high indices - that drops the dense buffer and every later
 * access has to go through element-property lookup. */
var SPARSE = [];
SPARSE[1000] = 1; SPARSE[1001] = 2; SPARSE[1002] = 3; SPARSE[1003] = 4;
SPARSE[1004] = 5; SPARSE[1005] = 6; SPARSE[1006] = 7; SPARSE[1007] = 8;

timeIt('sparse-array-read', N, function () {
  var s = 0;
  for (var i = 0; i < N; i++) s += SPARSE[1000 + (i & 7)];
  return s;
});

timeIt('sparse-array-write', N, function () {
  for (var i = 0; i < N; i++) SPARSE[1000 + (i & 7)] = i;
  return SPARSE[1000];
});

timeIt('sparse-array-read-huge', N, function () {
  var s = 0;
  for (var i = 0; i < N; i++) s += SPARSE[(i & 7) * 100000 + 3];
  return s;
});

/* --- string-key polymorphism: is the cost the path or IC thrash? ---------- */
var OK = { k0: 1, k1: 2, k2: 3, k3: 4, k4: 5, k5: 6, k6: 7, k7: 8 };
var KEY0 = 'k0', KEY1 = 'k1', KEY2 = 'k2', KEY3 = 'k3';

timeIt('strkey-mono', N, function () {
  var s = 0;
  for (var i = 0; i < N; i++) s += OK[KEY0];
  return s;
});

timeIt('strkey-poly2', N, function () {
  var s = 0, ks = [KEY0, KEY1];
  for (var i = 0; i < N; i++) s += OK[ks[i & 1]];
  return s;
});

timeIt('strkey-poly8', N, function () {
  var s = 0, ks = ['k0','k1','k2','k3','k4','k5','k6','k7'];
  for (var i = 0; i < N; i++) s += OK[ks[i & 7]];
  return s;
});

/* --- switch shapes: dense (jump-table candidate) vs sparse --------------- */
function makeDense(n) {
  var t = 0;
  for (var i = 0; i < N / 20; i++) {
    for (var k = 0; k < 20; k++) {
      switch (k % n) {
        case 0: t += 1; break;
        case 1: t += 2; break;
        case 2: t += 3; break;
        case 3: t += 4; break;
        case 4: t += 5; break;
        case 5: t -= 1; break;
        case 6: t ^= 1; break;
        case 7: t += 8; break;
        case 8: t += 9; break;
        case 9: t -= 2; break;
        default: t += 0; break;
      }
    }
  }
  return t;
}
timeIt('switch-dense10', N, function () { return makeDense(10); });

function makeSparse() {
  var t = 0;
  for (var i = 0; i < N / 20; i++) {
    for (var k = 0; k < 20; k++) {
      switch (k) {
        case 0: t += 1; break;
        case 1: t += 2; break;
        case 2: t += 3; break;
        case 3: t += 4; break;
        case 4: t += 5; break;
        case 5: t -= 1; break;
        case 6: t ^= 1; break;
        case 7: t += 8; break;
        case 8: t += 9; break;
        case 9: t -= 2; break;
        default: t += 0; break;
      }
    }
  }
  return t;
}
timeIt('if-chain10', N, function () {
  var t = 0;
  for (var i = 0; i < N / 20; i++) {
    for (var k = 0; k < 20; k++) {
      var v = k % 10;
      if (v === 0) t += 1; else if (v === 1) t += 2; else if (v === 2) t += 3;
      else if (v === 3) t += 4; else if (v === 4) t += 5; else if (v === 5) t -= 1;
      else if (v === 6) t ^= 1; else if (v === 7) t += 8; else if (v === 8) t += 9;
      else if (v === 9) t -= 2;
    }
  }
  return t;
});

/* --- 40-case switch: the size real bytecode VMs use (QIMEI/adtracker) ----- */
timeIt('switch-dense40', N, function () {
  var t = 0;
  for (var i = 0; i < N / 20; i++) {
    for (var k = 0; k < 20; k++) {
      switch (k * 2) {
        case 0: t += 1; break;
        case 1: t += 2; break;
        case 2: t += 3; break;
        case 3: t += 4; break;
        case 4: t += 5; break;
        case 5: t += 6; break;
        case 6: t += 7; break;
        case 7: t += 8; break;
        case 8: t += 9; break;
        case 9: t += 10; break;
        case 10: t += 11; break;
        case 11: t += 12; break;
        case 12: t += 13; break;
        case 13: t += 14; break;
        case 14: t += 15; break;
        case 15: t += 16; break;
        case 16: t += 17; break;
        case 17: t += 18; break;
        case 18: t += 19; break;
        case 19: t += 20; break;
        case 20: t += 21; break;
        case 21: t += 22; break;
        case 22: t += 23; break;
        case 23: t += 24; break;
        case 24: t += 25; break;
        case 25: t += 26; break;
        case 26: t += 27; break;
        case 27: t += 28; break;
        case 28: t += 29; break;
        case 29: t += 30; break;
        case 30: t += 31; break;
        case 31: t += 32; break;
        case 32: t += 33; break;
        case 33: t += 34; break;
        case 34: t += 35; break;
        case 35: t += 36; break;
        case 36: t += 37; break;
        case 37: t += 38; break;
        case 38: t += 39; break;
        case 39: t += 40; break;
        default: t += 0; break;
      }
    }
  }
  return t;
});

timeIt('if-chain40', N, function () {
  var t = 0;
  for (var i = 0; i < N / 20; i++) {
    for (var k = 0; k < 20; k++) {
      var v = k * 2;
      if (v === 0) t += 1; else if (v === 2) t += 3; else if (v === 4) t += 5;
      else if (v === 6) t += 7; else if (v === 8) t += 9; else if (v === 10) t += 11;
      else if (v === 12) t += 13; else if (v === 14) t += 15; else if (v === 16) t += 17;
      else if (v === 18) t += 19; else if (v === 20) t += 21; else if (v === 22) t += 23;
      else if (v === 24) t += 25; else if (v === 26) t += 27; else if (v === 28) t += 29;
      else if (v === 30) t += 31; else if (v === 32) t += 33; else if (v === 34) t += 35;
      else if (v === 36) t += 37; else if (v === 38) t += 39;
    }
  }
  return t;
});

/* --- sparse 40-case switch (arbitrary case values, the real VM shape) ---- */
var SPARSE_CASES = [3, 11, 17, 29, 41, 58, 63, 77, 90, 101, 118, 131, 149, 166, 177, 190, 203, 221, 237, 251, 269, 283, 299, 311, 337, 349, 367, 383, 401, 419, 433, 449, 467, 491, 509, 523, 547, 563, 587, 601];
timeIt('switch-sparse40', N, function () {
  var t = 0;
  for (var i = 0; i < N / 20; i++) {
    for (var k = 0; k < 20; k++) {
      switch (SPARSE_CASES[k * 2]) {
        case 3: t += 1; break;
        case 11: t += 2; break;
        case 17: t += 3; break;
        case 29: t += 4; break;
        case 41: t += 5; break;
        case 58: t += 6; break;
        case 63: t += 7; break;
        case 77: t += 8; break;
        case 90: t += 9; break;
        case 101: t += 10; break;
        case 118: t += 11; break;
        case 131: t += 12; break;
        case 149: t += 13; break;
        case 166: t += 14; break;
        case 177: t += 15; break;
        case 190: t += 16; break;
        case 203: t += 17; break;
        case 221: t += 18; break;
        case 237: t += 19; break;
        case 251: t += 20; break;
        case 269: t += 21; break;
        case 283: t += 22; break;
        case 299: t += 23; break;
        case 311: t += 24; break;
        case 337: t += 25; break;
        case 349: t += 26; break;
        case 367: t += 27; break;
        case 383: t += 28; break;
        case 401: t += 29; break;
        case 419: t += 30; break;
        case 433: t += 31; break;
        case 449: t += 32; break;
        case 467: t += 33; break;
        case 491: t += 34; break;
        case 509: t += 35; break;
        case 523: t += 36; break;
        case 547: t += 37; break;
        case 563: t += 38; break;
        case 587: t += 39; break;
        case 601: t += 40; break;
        default: t += 0; break;
      }
    }
  }
  return t;
});
