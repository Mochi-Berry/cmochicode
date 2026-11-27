// restparams.js — rest params + destructuring defaults.
function pick(...args) { return args[0] + 1; }
console.log("rest", pick(1, 2, 3));
function sum(...ns) { var t = 0; for (var x of ns) { t += x; } return t; }
console.log("sum", sum(1, 2, 3, 4));
const add = (...a) => a[0] + a[1];
console.log("arrow-rest", add(3, 4));
function mixed(a, ...r) { return a + r[0] + r.length; }
console.log("mixed", mixed(10, 1, 2));
var cfg = { a: 1, b: 2 };
var { a: aa, z: zz = 99 } = cfg;
console.log("destr-default", aa, zz);
