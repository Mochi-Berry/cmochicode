// dynamic_typing.js — the hybrid static/dynamic typing design in C99.
// A statically-shaped object keeps its typed struct fields (fast path) until a
// value of an incompatible type is assigned; then the field widens to a
// dynamic mochi_val (tagged union) so the value is preserved, not coerced.
var o = { a: 1 };           // 'a' starts as a static int field
o.a = "str";                // number field given a string -> widens to mochi_val
console.log("str-write", o.a);
o.a = true;                 // already dynamic, stays dynamic
console.log("bool-write", o.a);
o.a = 2.5;
console.log("double-write", o.a);
o.a = 42;
console.log("num-write", o.a);

// a string field given a number widens too
var s = { name: "berry" };
s.name = 99;
console.log("str->num", s.name);

// numeric promotion stays static: int field accepts a double exactly
var p = { n: 3 };
p.n = 1.5;
console.log("promote", p.n);

// a dynamically added field survives a later literal of the same shape
var d = { a: 1 };
d.x = 5;                    // dynamic field add
var e = { a: 2 };           // same shape: reorder must NOT drop d.x
console.log("dyn-add", d.x, e.a);

// mixed-type writes inside a function (same rule per call-site)
function fill(q) {
    q.v = "was-number";
    console.log("fn-write", q.v);
    q.v = 7;
    console.log("fn-write2", q.v);
}
var q = { v: 1 };
fill(q);
console.log("full", o, s, p, d, q);
