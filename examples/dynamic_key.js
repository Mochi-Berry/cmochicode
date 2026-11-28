// dynamic_key.js — `obj[k]` with a RUNTIME string key.
// The key is not a compile-time constant, so the access lowers to a per-shape
// dispatch helper (static key check + mochi_val boxing) instead of a direct
// struct field access. This is the dynamic-typing path next to the static one.
var o = { a: 1, b: "hi", c: true };
var key = "a";
console.log("read", o[key]);
key = "b";
console.log("readstr", o[key]);
key = "c";
console.log("readbool", o[key]);
// write through a runtime key
o[key] = 42;
console.log("still-a", o.a);
key = "a";
o[key] = 99;
console.log("write2", o.a);
// unknown key: reads as undefined, write is a no-op (the shape has no such field)
var unknown = "zzz";
console.log("unknown", o[unknown]);
o[unknown] = 7;
console.log("still-a2", o.a);
// static keys keep the fast direct-field path
console.log("static", o.a, o.b);
for (var i = 0; i < 2; i++) { o["a"] = o["a"] + 1; }
console.log("incr", o.a);
