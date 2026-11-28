// nested_dynamic.js — nested objects keep their shape, dynamic values unbox.
// A nested object literal gets its own shape, so the field is typed
// `struct X *`: property access on it and recursive console.log both work.
// A value read through a dynamic key is a mochi_val; arithmetic on it
// unboxes to a double (the result is typed double too, so the printf
// specifier matches the vararg type).
var o = { inner: { x: 1, y: "s" }, list: [1, 2] };
console.log("nested", o);
console.log("deep", o.inner.x, o.inner.y);

var d = { a: 1 };
var k = "n";
d[k] = 5;
console.log("arith", d[k] + 1, d[k] * 2, d[k] - 3);
console.log("read", d[k], d.a);