/* Array API lengkap: shift/unshift/splice/slice/concat/sort/fill/at/flat,
 * Object.entries, Array.from, Array.isArray. */
var a = [3, 1, 2];
a.push(9); a.pop();
console.log("push/pop", a.join("-"));

a.shift();
console.log("shift", a.join("-"));
a.unshift(0);
console.log("unshift", a.join("-"));

var removed = a.splice(1, 2, 7, 8);
console.log("splice removed", removed.join("-"), "kept", a.join("-"));

console.log("slice", a.slice(1).join("-"), a.slice(0, 1).join("-"));
console.log("concat", a.concat([5, 6]).join("-"));
console.log("sort", a.slice().sort().join("-"));
console.log("fill", a.slice().fill(0, 1).join("-"));
console.log("at", a.at(-1), a.at(0), a.at(99));
console.log("lastIndexOf", a.lastIndexOf(7));
console.log("indexOf", a.indexOf(8), "includes", a.includes(2));

var flat = [1, [2, 3], 4].flat();
console.log("flat", flat.join("-"));

var entries = Object.entries(a);
console.log("entries", entries.length, entries[0].join(":"), entries[1].join(":"));
var eo = Object.entries({ x: 1, y: 2 });
console.log("entries obj", eo.length, eo[1].join(":"));

console.log("from", Array.from(a).join("-"));
console.log("isArray", Array.isArray(a), Array.isArray("no"), Array.isArray(flat));

var mapped = a.map(x => x * 2).filter(x => x > 5);
console.log("map/filter", mapped.join("-"));
console.log("reduce", a.reduce((s, x) => s + x, 0));
console.log("reverse", a.slice().reverse().join("-"));