/* Real-playback state probe: play, pause, seek, resume.
 *
 * Runs against tools/pages/cap_media_real.html with ?src=... (and
 * &kind=audio). The report shows that the clock is driven by the decoder
 * (`real=1`), that it advances at wall-clock rate while playing, that it
 * freezes while paused, and that a seek lands on the requested position. */
(function () {
  var el = (location.search.indexOf('kind=audio') >= 0)
    ? document.getElementById('a')
    : document.getElementById('v');

  var out = [];
  var startedAt = 0;
  var ctAtPlay = 0;
  var ctAtPause = 0;

  function now() { return (new Date()).getTime() / 1000; }

  function snap(tag) {
    out.push(tag + ': ct=' + el.currentTime.toFixed(3)
      + ' paused=' + (el.paused ? 1 : 0)
      + ' ended=' + (el.ended ? 1 : 0)
      + ' real=' + el._agbRealPlayback
      + ' rs=' + el.readyState
      + ' dur=' + el.duration.toFixed(2));
  }

  setTimeout(function () {
    ctAtPlay = el.currentTime;
    startedAt = now();
    el.play();

    setTimeout(function () {
      out.push('play: advanced=' + (el.currentTime - ctAtPlay).toFixed(3)
        + ' elapsed=' + (now() - startedAt).toFixed(2));
      snap('afterPlay');

      ctAtPause = el.currentTime;
      el.pause();

      setTimeout(function () {
        snap('paused');
        out.push('pauseDrift=' + (el.currentTime - ctAtPause).toFixed(3));

        var target = el.duration > 0 ? Math.min(el.duration / 3, 60) : 10;
        var seekStarted = now();

        el.currentTime = target;

        /* A seek restarts the decoder, so this call is allowed to be slow-ish
         * but must not sit out a thread timeout (it used to take seconds). */
        out.push('seekMs=' + Math.round((now() - seekStarted) * 1000));
        out.push('seekTo=' + target.toFixed(3));

        setTimeout(function () {
          snap('afterSeek');

          el.play();

          setTimeout(function () {
            out.push('resumed: advanced=' + (el.currentTime - target).toFixed(3));
            snap('resumed');
            probeExit(out.join(' || '));
          }, 700);
        }, 700);
      }, 600);
    }, 1500);
  }, 200);
})();
