/* Deferred classic script: still its own element, and it runs after parsing. */
window.__deferId = document.currentScript ? document.currentScript.id : 'null';
window.__deferData = document.currentScript ? document.currentScript.getAttribute('data-mark') : 'null';
