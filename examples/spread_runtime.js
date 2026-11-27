// spread_runtime.js — call spread of a RUNTIME array + string return/params.
// The `...arr` arg has a runtime-dependent arity: it is materialized into a
// mochi_array and each callee parameter position is read with a bounds guard.
function s(a, b, c) { return a + b + c; }
var arr = [2, 3];
console.log("prefix", s(10, ...arr));
console.log("middle", s(...arr, 100));
console.log("more", s(...[1, 2], 5));
function rest(...args) { return args.length; }
console.log("restlen", rest(1, ...arr, 9));
console.log("log", ...[7, 8]);
// string params/return (concat inference, not just literals)
function p2(x, y) { return x + "|" + y; }
var strs = ["a", "b"];
console.log("str", p2(...strs));
function p1(x) { return x + "!"; }
console.log("p1", p1("hi"));
