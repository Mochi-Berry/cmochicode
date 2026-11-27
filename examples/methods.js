// methods.js — computed props, for...of, shorthand methods.
var o = { ["dyn"]: 42, x: 1 };
console.log("computed", o.dyn, o.x);
var arr = [1, 2, 3];
var s = 0;
for (var v of arr) { s += v; }
console.log("forof", s);
var obj2 = { f() { return 7; }, g(x) { return x * 3; } };
console.log("method", obj2.f(), obj2.g(5));
var names = ["a", "b"];
for (var nm of names) { console.log("name", nm); }
