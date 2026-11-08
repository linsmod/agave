// tools/pages/check_xhr_frame_cancel.js
// S2 regression: the frame navigated away before /slow-xhr answered, so its
// callback must never run. fired=1 would mean a detached document's callback
// fired (pre-S2 behaviour).
probeExit('fired=' + window.__fired + ' failed=' + window.__failed);
