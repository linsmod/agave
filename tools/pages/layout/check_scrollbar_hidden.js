/* Report for scrollbar_hidden.html: ::-webkit-scrollbar{display:none} must
 * remove the document scrollbar from layout (client == inner) and
 * ::-webkit-scrollbar{width:0} must do the same for a scroll container. */
var out = [];
var de = document.documentElement;
out.push("viewport client=" + de.clientWidth + "x" + de.clientHeight +
         " inner=" + window.innerWidth + "x" + window.innerHeight);
out.push("doc scroll=" + de.scrollWidth + "x" + de.scrollHeight);
out.push("full width=" + Math.round(document.getElementById("full").getBoundingClientRect().width));
var box = document.getElementById("box");
out.push("box clientWidth=" + box.clientWidth + " offsetWidth=" + box.offsetWidth);
probeExit(out.join("\n"));
