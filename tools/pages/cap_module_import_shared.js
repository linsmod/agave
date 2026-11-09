// Shared by the module/import regressions below. Each realm that evaluates it
// gets its own instance: `window.__sharedLoads` starts at 0 per window, so
// bump() returning 1 proves this realm ran the module body exactly once.
window.__sharedLoads = (window.__sharedLoads || 0) + 1;

export function bump() {
    return window.__sharedLoads;
}
