/* Real-playback probe.
 *
 * The headless shell gives one --eval and the page's timers only run while
 * the frame loop turns, so the sequence is staged from setTimeout: start
 * playback, let it run for ~1.6 s, then report. The report includes
 * `real=` (1 when a decoder drives the element) and how far currentTime
 * actually advanced - which is what "real playback" has to mean.
 */
(function () {
  var el = (location.search.indexOf('kind=audio') >= 0)
    ? document.getElementById('a')
    : document.getElementById('v');

  var startedAt = 0;
  var before = 0;

  function step1() {
    before = el.currentTime;
    startedAt = (new Date()).getTime();
    var p = el.play();

    /* play() must hand back a Promise (players chain .catch on it). */
    var isPromise = !!(p && typeof p.then === 'function');

    setTimeout(function () {
      var elapsed = ((new Date()).getTime() - startedAt) / 1000;
      var advanced = el.currentTime - before;

      probeExit('promise=' + isPromise
        + ' elapsed=' + elapsed.toFixed(2)
        + ' advanced=' + advanced.toFixed(3)
        + ' ' + window.report('started'));
    }, 1600);
  }

  setTimeout(step1, 200);
})();
