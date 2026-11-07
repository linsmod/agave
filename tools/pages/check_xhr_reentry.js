/* Observer for xhr_reentry.html: two deliveries, one nested evaluation.
 * Needles live in tools/run_xhr_tests.ps1. */
probeExit(['done=' + window.__done,
           'nested=' + window.__nested,
           'log=' + (window.__log || []).join(','),
           'ready=' + document.readyState].join(' '));
