/* Report for css_background_override.html (R195). Run through the probe:
 *
 *   agave_shell.exe --probe tools/pages/css_background_override.html \
 *       --eval-file tools/pages/check_background_override.js
 *
 * One line per case: the background-image KIND (a gradient and a url() are the
 * two things that can sit on a layer) plus the computed colour, so the runner
 * can diff against Chrome without depending on either engine's serialization of
 * the gradient itself.
 */
function kind(v) {
  if (!v || v === 'none') return 'none';
  if (v.indexOf('url(') === 0) return 'url';
  if (v.indexOf('gradient(') >= 0) return 'gradient';
  return 'other';
}

var out = [];
['c1', 'c2', 'c3', 'c4', 'c5', 'c6'].forEach(function (id) {
  var el = document.getElementById(id);
  var cs = getComputedStyle(el);
  out.push(id + ' img=' + kind(cs.backgroundImage) + ' bg=' + cs.backgroundColor);
});

probeExit(out.join('\n'));
