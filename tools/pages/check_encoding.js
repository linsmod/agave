/* Encoding checks for tools/pages/encoding/*.html (run by run_encoding_tests.ps1).
 *
 * Each page carries its own expectations:
 *   <meta name="expect-title"   content="...">
 *   <meta name="expect-charset" content="UTF-8|GBK">
 *
 * The page is served with a deliberately conflicting HTTP header / <meta> /
 * BOM combination, so a matching title means the priority rules held (R164).
 * The probe exits 0 on success, 7 on a mismatch. */
(function () {
    function meta(name) {
        var el = document.querySelector('meta[name="' + name + '"]');
        return el ? el.getAttribute('content') : '';
    }

    var title = document.title;
    var charset = document.characterSet || '';
    var expectTitle = meta('expect-title');
    var expectCharset = meta('expect-charset');

    var titleOk = (title === expectTitle);
    var charsetOk = (charset.toUpperCase() === expectCharset.toUpperCase());
    var bodyOk = document.body ? (document.body.textContent.indexOf('正文') >= 0) : false;
    var ok = titleOk && charsetOk && bodyOk;

    console.log('ENC-' + (ok ? 'PASS' : 'FAIL') +
                ' title="' + title + '" charset="' + charset +
                '" body=' + (bodyOk ? 'ok' : 'bad') +
                ' expectTitle="' + expectTitle + '" expectCharset="' + expectCharset + '"');

    if (!ok) {
        console.log('ENC-DETAIL titleOk=' + titleOk + ' charsetOk=' + charsetOk +
                    ' inputEncoding=' + document.inputEncoding + ' charsetAttr=' + document.charset);
    }

    probeExit(ok ? 0 : 7);
})();
