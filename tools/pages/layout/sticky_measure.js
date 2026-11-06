/* Geometry half of the sticky regression: prints `key=value` pairs that
 * tools/run_sticky_tests.ps1 asserts. Runs in the probe (`--eval-file`). */
(function () {
  var head = document.getElementById('head');
  var foot = document.getElementById('foot');
  var box = document.getElementById('box');
  var s = document.getElementById('s');

  function top(el) { return Math.round(el.getBoundingClientRect().top); }
  function out(list) { probeExit(list.join(' ')); }

  var res = [];
  res.push('pos=' + getComputedStyle(head).position);
  res.push('h0=' + top(head));
  res.push('f0=' + top(foot));

  window.scrollTo(0, 500);
  res.push('h500=' + top(head));
  res.push('f500=' + top(foot));
  res.push('hit=' + (function () {
    var el = document.elementFromPoint ? document.elementFromPoint(10, 10) : null;
    return el ? (el.id || el.tagName) : 'none';
  })());

  var maxScroll = document.documentElement.scrollHeight - window.innerHeight;
  window.scrollTo(0, maxScroll);
  res.push('hmax=' + top(head));
  res.push('fmax=' + top(foot));
  res.push('fmaxNatural=' + Math.round(foot.offsetTop - window.scrollY));

  /* Inner scroller: the sticky bar must stay at the box's top edge. */
  res.push('s0=' + (top(s) - top(box)));
  box.scrollTop = 100;
  res.push('s100=' + (top(s) - top(box)));
  box.scrollTop = 400;
  res.push('s400=' + (top(s) - top(box)));

  out(res);
})();
