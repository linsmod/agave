/* Probe companion: left-click a row of the inspector's in-page menu.
 *
 * Sequence driven from the host (see tools/run_inspector_tests.ps1):
 *   right-click at (120,140) -> menu opens at (132,154)
 *   left-click  at (226,171) -> that is the first row ("Copy element info")
 *
 * Reports "row clicked" once the menu closes after having been open, so the
 * host can assert the click landed on a row instead of dismissing the menu.
 */
(function () {
  var wasOpen = 0;
  var reported = 0;

  var timer = setInterval(function () {
    var ins = window.__agaveInspector;

    if (!ins || reported) return;

    if (ins.menuOpen()) {
      wasOpen = 1;
      return;
    }
    if (wasOpen) {
      reported = 1;
      clearInterval(timer);
      setTimeout(function () { probeExit('row clicked'); }, 300);
    }
  }, 100);

  setTimeout(function () {
    if (!reported) probeExit('no click (menuOpen=' + (window.__agaveInspector ? window.__agaveInspector.menuOpen() : 'gone') + ')');
  }, 8000);
}());
