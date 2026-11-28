// ecma_arrays.js — higher-order array methods with inline arrows (no closures).
var a = [1, 2, 3, 4];
var d = a.map(x => x * 2);
console.log("map", d);
var f = a.filter(x => x > 2);
console.log("filter", f);
var s = a.reduce((acc, x) => acc + x, 0);
console.log("reduce", s);
a.forEach(x => console.log("el", x));
