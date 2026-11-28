// dynamic_map.js — the dynamic overflow map for object keys that only appear
// at runtime. A shape with a runtime (non-constant) key keeps its typed static
// fields as the fast path, but every key added afterwards — literal or runtime
// — goes into a lazily allocated mochi_map so JS insertion order, string
// quoting and console.log inspection all match node.
var d = { a: 1, b: "x" };
var k = "newkey";
d[k] = 5;                 // runtime key -> overflow map
d[k] = "str";             // same key updates the map entry
d["other"] = true;        // literal key added LATER -> also the map (JS order)
console.log("inspect", d);

console.log("read", d.a, d[k], d["other"]);

// a second object of the same literal shape is independent (per-instance
// shapes): its own map, no cross-talk.
var e = { a: 1, b: "x" };
e["q"] = "hi";
console.log("e", e, "d unchanged", d);