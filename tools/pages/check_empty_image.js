/* Regression for R140: an image that finished with no body used to crash the
 * engine (ImageSource::setData dereferenced a null SharedBuffer). The document
 * has to stay alive, all three broken images have to end up complete with
 * naturalWidth 0, and the valid image next to them has to decode normally.
 *
 * Runs at eval time (the probe evaluates after the load event), so it exits
 * synchronously: a timer here would land after the probe's settle window.
 * Run with tools/empty_image_server.py; probe exits 0 and prints the payload. */
(function () {
    var ids = ['empty', 'nocontent', 'truncated', 'ok'], out = [];
    for (var i = 0; i < ids.length; i++) {
        var img = document.getElementById(ids[i]);
        out.push(ids[i] + '=' + (img ? (img.complete ? 1 : 0) : 'missing') +
                 '/' + (img ? img.naturalWidth : -1));
    }
    probeExit(out.join(' '));
})();
