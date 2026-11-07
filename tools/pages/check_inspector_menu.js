/* Probe: inspector.js element context menu (in-process assertions).
 *
 * Assumes the shell ran `--inject inspector` on tools/pages/inspector_demo.html.
 * #target is at document (100,120) 240x80 with text "target".
 *
 * The last copy performed here is "Copy text", so the caller can assert the
 * Win32 clipboard holds "target" after the process exits; the element-info
 * JSON is checked inline (kind/selector/rects/scroll).
 */
(function () {
  var out = [];
  var ins = window.__agaveInspector;

  function push(k, v) { out.push(k + '=' + String(v)); }

  if (!ins) {
    probeExit('inspector missing');
    return;
  }

  var el = ins.inspectAt(120, 140);
  push('hover', el ? String(el.tagName).toLowerCase() + '#' + el.id : 'none');

  push('labels', (ins.menuAt(120, 140) || []).join('|'));

  /* Menu rows must be hit-testable: elementFromPoint over row 0 has to be our
   * own node, or a real click would land on the page under the menu. */
  var hit = document.elementFromPoint(226, 170);
  push('row0', hit ? String(hit.tagName).toLowerCase() + ':' + String(hit.textContent) : 'none');
  push('row0inside', (function () {
    for (var n = hit; n; n = n.parentNode) if (n === ins.root()) return 1;
    return 0;
  }()));
  push('menuClosedOnSelect', ins.menuSelect('Copy selector') ? (ins.menuOpen() ? 'no' : 'yes') : 'notfound');

  /* Element info JSON (what an agent receives): identity + both rects. */
  var info = ins.elementInfo() || '';
  push('infoKind', info.indexOf('"kind":"agave-element"') >= 0 ? 1 : 0);
  push('infoSelector', info.indexOf('"selector":"div#target"') >= 0 ? 1 : 0);
  push('infoRect', info.indexOf('"getBoundingClientRect":{"left":100,"top":120,"width":240,"height":80}') >= 0 ? 1 : 0);
  push('infoDocRect', info.indexOf('"getBoundingClientRectInPage":{"left":100,"top":120') >= 0 ? 1 : 0);
  push('infoViewport', info.indexOf('"inViewport":true') >= 0 ? 1 : 0);

  /* Select parent walks up one element and closes the menu. */
  ins.menuAt(120, 140);
  push('parent', ins.menuSelect('Select parent') ? String(ins.element().tagName).toLowerCase() : 'notfound');

  /* Synthetic contextmenu: the handler must cancel it (the same
   * preventDefault stops ContextMenuController -> the shell's native menu on
   * the engine path) and open the menu on the element under the point. */
  var prevented = 0;
  document.dispatchEvent({
    type: 'contextmenu',
    clientX: 120, clientY: 140, button: 2,
    preventDefault: function () { prevented = 1; },
    stopPropagation: function () {}
  });
  push('prevented', prevented);
  push('menuOpen', ins.menuOpen() ? 1 : 0);
  push('copyText', ins.menuSelect('Copy text') ? 1 : 0);

  ins.menuAt(120, 140);
  push('exited', ins.menuSelect('Exit inspector') ? 1 : 0);
  push('gone', (window.__agaveInspector === null || window.__agaveInspector === undefined) ? 1 : 0);

  probeExit(out.join(' '));
}());
