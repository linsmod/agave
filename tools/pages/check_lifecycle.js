/* Observer for cap_lifecycle.html: dump what the page recorded at each stage.
 * Needles live in tools/run_layout_tests.ps1 ("DOM lifecycle" group). */
probeExit((window.__obs || []).concat(['appended=' + window.__appended]).join(' '));
