// json_stringify.js — JSON.stringify for shaped objects in C99.
// Every shape gets a JSON formatter: nested objects recurse through their
// own formatter, arrays through mochi_array_json, and the dynamic overflow
// map entries are included — so a runtime-added key shows up in the JSON
// exactly like in node.
var o = { a: 1, b: "x", c: true, d: { e: 2 }, f: [1, 2] };
console.log(JSON.stringify(o));

var p = { q: 1 };
p["r"] = "z";                // dynamic key -> included in the JSON
console.log(JSON.stringify(p));

// a string with a quote gets escaped like JSON does
var s = { t: "he said \"hi\"" };
console.log(JSON.stringify(s));