// object_api.js — the dynamic object API in C99.
// `key in obj` is a runtime membership test over the shape's fields plus its
// overflow map; Object.keys / Object.values enumerate in JS insertion order;
// Object.assign copies a source's static fields and dynamic keys into a
// target, using the same dispatch the rest of the dynamic path uses.
var o = { a: 1, b: "x" };
o["newkey"] = 5;

console.log("in-static", "a" in o);
console.log("in-map", "newkey" in o);
console.log("in-missing", "zzz" in o);

console.log("keys", Object.keys(o).join(","));
console.log("values", Object.values(o).join(","));

// Object.assign: the target gains the source's fields and dynamic keys
var p = { q: 1 };
Object.assign(p, o);
console.log("assign", p.q, p.a, p.b, p.newkey);