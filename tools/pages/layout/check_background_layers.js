/* Computed-value side of the multi-layer background fixture. The pixel side is
 * asserted by tools/run_layout_tests.ps1; each `key=value` below is a needle
 * there. */
(function () {
  var out = [];
  var gl = document.getElementById('gl');
  var cs = getComputedStyle(gl);

  /* Both layers survive: "linear-gradient(...), linear-gradient(...)". */
  out.push('bg.image=' + (cs.backgroundImage.split('linear-gradient').length - 1));
  out.push('bg.clip=' + cs.backgroundClip);
  out.push('bg.origin=' + cs.backgroundOrigin);

  var shadow = getComputedStyle(document.getElementById('shadow')).boxShadow;
  out.push('bg.shadowSpread=' + (shadow.indexOf('6px') >= 0 ? 'ok' : shadow));

  var ta = document.getElementById('ta');
  out.push('bg.placeholderAttr=' + (ta.getAttribute('placeholder') === 'MMMMMM' ? 'ok' : 'bad'));

  /* devicePixelRatio is a bare global in every browser; the engine used to
   * throw ReferenceError on it. */
  out.push('bg.dpr=' + (typeof devicePixelRatio === 'number' && devicePixelRatio > 0 ? 'ok' : 'bad'));

  probeExit(out.join(' '));
})();
