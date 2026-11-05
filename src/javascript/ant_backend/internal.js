/*
 * internal.js - the browser's own API for internal pages (agave://...).
 *
 * Evaluated only for an internal document (see AntRuntime::bindDocument); a web
 * page never has __agbDownloadList and therefore never gets `window.agave`.
 * That separation is the point: the panel below lists the user's file paths and
 * can cancel transfers, which is a capability, not a DOM feature.
 *
 * Runtime asset next to the binary (AGB_INTERNAL overrides the path), so it can
 * be edited without relinking, same as prelude.js / polyfills.js.
 */
(function (window) {
  if (typeof __agbDownloadList !== 'function')
    return;

  var api = {
    /* Snapshot of every download job the process knows about. */
    downloads: function () { return __agbDownloadList(); },
    cancelDownload: function (id) { return __agbDownloadCancel(id) > 0; },
    openDownload: function (id) { return __agbDownloadOpen(id) > 0; },
    /* Assign a function to redraw when a job changes. */
    onchange: null
  };

  window.agave = api;

  window.addEventListener('agavedownloadchange', function () {
    if (typeof api.onchange === 'function')
      api.onchange();
  });
})(window);
