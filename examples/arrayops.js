// arrayops.js — array methods + pad + function.length.
var a = [1, 2, 3, 4];
console.log("join", a.join("-"));
console.log("includes", a.includes(3));
console.log("indexOf", a.indexOf(4));
console.log("slice", a.slice(1, 3));
console.log("reverse", [1, 2, 3].reverse());
console.log("find", a.find(x => x > 2));
console.log("some", a.some(x => x > 3));
console.log("every", a.every(x => x > 0));
console.log("pad", "7".padStart(3, "0"), "7".padEnd(3, "0"));
function two(a, b = 2, c) { return a; }
console.log("fnlen", two.length);
