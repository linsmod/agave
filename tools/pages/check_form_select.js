/* Read back the <select> state after the probe drove it (see the select groups
 * in run_layout_tests.ps1): the menu was opened with --click and an item was
 * picked with either {down}{enter} or a second --click. */
var s = document.getElementById('s');

probeExit('v=' + s.value +
          ';i=' + s.selectedIndex +
          ';n=' + s.length +
          ';t=' + s.options[2].text +
          ';type=' + s.type +
          ';log=' + document.getElementById('log').textContent);
