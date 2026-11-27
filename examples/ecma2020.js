// ecma2020.js — logical assignment, numeric separator, optional chaining,
// nullish coalescing, default params, string methods (ECMA 2020+).
var count = 0;
count ||= 42;
console.log("count ||=", count);
var name = null;
name ??= "anon";
console.log("name ??=", name);
var n = 1_000_000;
var hex = 0xFF_FF;
console.log("sep", n, hex);
var s = "Hello Berry";
console.log("includes", s.includes("Berry"));
console.log("startsWith", s.startsWith("Hello"));
console.log("endsWith", s.endsWith("Berry"));
console.log("slice", s.slice(6));
console.log("replace", s.replace("Hello", "Hi"));
console.log("split", s.split(" "));
console.log("indexOf", s.indexOf("Berry"));
var f = (a, b = 10) => (a ?? 0) * 2 + b;
console.log("default", f(3));
try { throw "x"; } catch (e) { console.log("optional catch ok", e); }
