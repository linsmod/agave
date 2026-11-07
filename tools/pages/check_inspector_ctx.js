/* Probe companion: real right-click on the (hidden) probe control.
 *
 * Registered after `--inject inspector`. The shell posts WM_RBUTTONDOWN/UP to
 * the hidden AgaveWebViewControl window; the engine turns it into a DOM
 * contextmenu event, which reaches both the inspector's listener (injected
 * earlier, so it runs first and opens the in-page menu) and this one.
 *
 * Reports the labels the inspector opened with, or "closed" if the inspector
 * never saw the event. The matching shell-side check is that NO
 * "[probe] context menu at ..." line appears: that callback is the native
 * menu, and the inspector's preventDefault must suppress it.
 */
(function () {
  var fired = 0;

  document.addEventListener('contextmenu', function () {
    var ins = window.__agaveInspector;
    var labels = (ins && ins.menuOpen()) ? ins.menuLabels().join('|') : 'closed';

    fired = 1;
    probeExit('ctx labels=' + labels);
  }, false);

  setTimeout(function () {
    if (!fired) probeExit('ctx not fired');
  }, 8000);
}());
