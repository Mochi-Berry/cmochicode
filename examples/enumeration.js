// enumeration.js — runtime object enumeration in C99.
// for-in over an object and Object.keys(obj) both walk the shape's static
// field names first, then the dynamic overflow map keys, in JS insertion
// order (the _keys helper builds that list at runtime).
var o = { a: 1, b: "x" };
o["newkey"] = 5;            // dynamic key -> overflow map
o["other"] = "s";           // added later -> map keeps insertion order

var out = "";
for (var k in o) out += k + ":" + o[k] + " ";
console.log("forin", out);

console.log("keys", Object.keys(o).join(","));
console.log("count", Object.keys(o).length);

// a plain object with no dynamic keys enumerates its static fields only
var p = { i: 1 };
var u = "";
for (var k in p) u += k;
console.log("plain", u, Object.keys(p).join("|"));