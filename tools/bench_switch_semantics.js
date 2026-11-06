function eq(a, b, what) { if (a !== b) { console.log("FAIL " + what + ": " + a + " !== " + b); throw new Error(what); } }

// fallthrough + ordering
var s = "";
switch (2) { case 1: s += "1"; case 2: s += "2"; case 3: s += "3"; break; case 4: s += "4"; }
eq(s, "23", "fallthrough");

// default in the middle only runs when nothing matches
var d = "";
switch (9) { case 0: d += "a"; break; default: d += "d"; case 1: d += "b"; break; case 2: d += "c"; }
eq(d, "db", "default middle");

// string cases + no match
var t = "";
switch ("k1") { case "k0": t = "zero"; break; case "k1": t = "one"; break; default: t = "none"; }
eq(t, "one", "string case");

// strict equality: -0 matches 0, NaN matches nothing
var z = ""; switch (-0) { case 0: z = "zero"; break; default: z = "other"; } eq(z, "zero", "-0 === 0");
var n = ""; switch (NaN) { case NaN: n = "nan"; break; default: n = "other"; } eq(n, "other", "NaN");

// side-effecting case expressions: evaluated in order, until a match, once each
var log = [];
function probe(v) { log.push(v); return v; }
var r = 0;
switch (3) { case probe(1): r = 1; break; case probe(3): r = 3; break; case probe(5): r = 5; break; }
eq(r, 3, "match value");
eq(log.join(","), "1,3", "side effects");

// nested switch inside loops with break/continue
var acc = 0;
for (var i = 0; i < 5; i++) {
  switch (i) {
    case 0: continue;
    case 2: acc += 10; break;
    default: acc += 1;
  }
  acc += 100;
}
eq(acc, 413, "loop interplay");

// no default, no match
var q = "x"; switch (42) { case 1: q = "one"; break; } eq(q, "x", "no match, no default");

console.log("ALL SWITCH SEMANTICS PASSED");
