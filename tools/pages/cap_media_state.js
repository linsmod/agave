/* Media element transport probe.
 *
 * The headless shell allows exactly one --eval, and page timers only advance
 * while the frame loop runs. So the checks are staged from the page's own
 * timers: each step runs, records its result, and the next step is scheduled
 * with setTimeout(). The final step calls probeExit() with the joined report.
 */
(function () {
  var steps = [];

  function push(name, fn) { steps.push({ name: name, fn: fn }); }

  push('loaded', function () { return window.reportLoaded(); });
  push('startPlayback', function () { return window.startPlayback(); });
  push('readPlayback', function () { return window.readPlayback(); });
  push('finishPlayback', function () { return window.finishPlayback(); });
  push('readEnded', function () { return window.readEnded(); });
  push('pause', function () { return window.pauseAndRead(); });

  var out = [];

  function run(i) {
    if (i >= steps.length) {
      probeExit(out.join(' || '));
      return;
    }

    var result;
    try {
      result = steps[i].fn();
    } catch (e) {
      result = 'THREW ' + e;
    }

    out.push(steps[i].name + ': ' + result);

    /* One frame is enough for the ticker; `ended` needs one tick after the
     * seek, so the last steps wait a little longer. */
    setTimeout(function () { run(i + 1); }, 320);
  }

  run(0);
})();
