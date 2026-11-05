/* Report for resize_probe.html. Run it through the probe with
 *   agave_shell.exe --probe tools/pages/resize/resize_probe.html --resize 500x400 \
 *       --eval-file tools/pages/resize/check_resize.js
 * and compare with the same run without --resize:
 *   innerWidth/innerHeight must follow the resize (layout + clientWidth),
 *   #pct (a percentage) must follow it too, and all three resize-event paths
 *   (addEventListener, window.onresize, <body onresize>) should have fired.
 *   #vw (100vw/50vh) and the @media breakpoint are the two things that do NOT
 *   follow a resize; printing them keeps that visible instead of guessing. */
var out = [];
var vw = document.getElementById('vw');
var pct = document.getElementById('pct');
var mq = document.getElementById('mq');
var mqWide = document.getElementById('mq-wide');
var de = document.documentElement;

function list(v) {
  return (v && v.length) ? v.join(',') : '(none)';
}

out.push("viewport inner=" + window.innerWidth + "x" + window.innerHeight +
         " client=" + de.clientWidth + "x" + de.clientHeight +
         " scroll=" + de.scrollWidth + "x" + de.scrollHeight);
out.push("vwBox=" + vw.offsetWidth + "x" + vw.offsetHeight +
         " (100vw x 50vh)");
out.push("pctBox=" + pct.offsetWidth + " (50%)");
out.push("mediaCSS=" + getComputedStyle(mq).backgroundColor +
         " (rgb(10,20,30) = desktop, rgb(200,30,40) = mobile rule)");
out.push("mediaCSSwide=" + getComputedStyle(mqWide).backgroundColor +
         " (rgb(30,200,40) = >=900px rule, rgb(40,30,20) = base)");
out.push("matchMedia(max-width:600px)=" +
         (window.matchMedia ? window.matchMedia('(max-width: 600px)').matches : 'n/a'));
out.push("resizeEvents addEventListener=" + list(window.__resizeLog) +
         " onresize=" + list(window.__propLog) +
         " bodyAttr=" + list(window.__attrLog));
probeExit(out.join("\n"));
