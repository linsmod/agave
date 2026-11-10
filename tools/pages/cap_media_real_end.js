/* Real-playback end probe: jump near the end and check the `ended` transition.
 *
 * The pipeline drives currentTime, so `ended` must come from the media clock
 * reaching the duration - not from a simulated ticker (this page is only
 * meaningful on a clip short enough to finish in the settle window). */
(function () {
  var el = (location.search.indexOf('kind=audio') >= 0)
    ? document.getElementById('a')
    : document.getElementById('v');

  var out = [];
  var log = [];

  ['ended', 'pause', 'seeking', 'seeked'].forEach(function (t) {
    el.addEventListener(t, function () { log.push(t); });
  });

  setTimeout(function () {
    if (!(el.duration > 0)) {
      probeExit('no duration: dur=' + el.duration + ' err=' + (el.error ? el.error.code : 'null'));
      return;
    }

    var target = Math.max(el.duration - 0.25, 0);

    el.currentTime = target;
    el.play();
    out.push('seekTo=' + target.toFixed(3) + ' dur=' + el.duration.toFixed(3));

    setTimeout(function () {
      out.push('ended=' + (el.ended ? 1 : 0)
        + ' paused=' + (el.paused ? 1 : 0)
        + ' ct=' + el.currentTime.toFixed(3)
        + ' real=' + el._agbRealPlayback
        + ' events=' + log.join(','));
      probeExit(out.join(' || '));
    }, 1500);
  }, 300);
})();
