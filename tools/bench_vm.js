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
