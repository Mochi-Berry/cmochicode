// spread.js — object spread {...src} merge + call spread of array literal.
function s(a, b, c) { return a + b + c; }
console.log("sum", s(...[1, 2, 3]));
var o2 = { x: 1, label: "hi" };
var m = { ...o2, y: 2 };
console.log("m", m.x, m.y, m.label);
var n2 = { ...o2 };
console.log("n2", n2.x, n2.label);
var merged = { ...{ p: 9, q: "z" }, r: true };
console.log("merged", merged.p, merged.q, merged.r);
