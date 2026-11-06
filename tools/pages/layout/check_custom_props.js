/* Custom property API surface (R86): what a page can read and write.
 * tools/run_layout_tests.ps1 asserts each `key=value` below. */
(function () {
  var el = document.getElementById('d');
  var root = document.documentElement;
  var out = [];

  /* values that come from the stylesheet keep the author's spelling */
  out.push('sheetTint=' + getComputedStyle(root).getPropertyValue('--tint'));
  out.push('sheetPad=' + getComputedStyle(root).getPropertyValue('--pad'));

  /* inline custom property round-trips through getPropertyValue/cssText */
  el.style.setProperty('--mine', 'rgb(1, 2, 3)');
  out.push('inlineOwn=' + el.style.getPropertyValue('--mine'));
  out.push('inlineCssText=[' + el.style.cssText + ']');

  /* ... and is visible to the cascade of the same element */
  el.style.color = 'var(--mine)';
  out.push('inlineColor=' + getComputedStyle(el).color);

  /* a custom property may reference another one */
  el.style.setProperty('--alias', 'var(--tint)');
  el.style.color = 'var(--alias)';
  out.push('chainedColor=' + getComputedStyle(el).color);

  /* removal */
  out.push('removed=[' + el.style.removeProperty('--alias') + ']');
  out.push('afterRemove=[' + el.style.getPropertyValue('--alias') + ']');

  probeExit(out.join(' '));
})();
