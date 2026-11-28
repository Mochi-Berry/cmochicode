// dynamic_ops.js — object spread from a function return + delete, in C99.
// A function that returns an object literal now carries that shape in its
// return type (struct <shape> *), so `{...mk()}` can copy the source's real
// fields instead of warning and dropping them. `delete` on a static field
// widens it to mochi_val and stores undefined; on a dynamic key it removes
// the entry from the overflow map.
function mk() { return { x: 1, y: 2 }; }
var c = { a: 0, ...mk() };
console.log("spread", c.a, c.x, c.y);

var o = { a: 1, b: 2 };
delete o.b;                 // static field -> undefined
console.log("static", o.a, o.b);
o.b = 9;                    // re-assign works after the delete
console.log("reassign", o.b);

var k = "c";
o[k] = 3;                   // runtime key -> overflow map
console.log("map-add", o[k]);
delete o[k];                // runtime key -> removed from the map
console.log("map-del", o[k]);
delete o["zzz"];            // never existed -> true, reads undefined
console.log("missing", o.zzz);