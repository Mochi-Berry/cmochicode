// forin.js — for...in over an object's own keys (compile-time field list).
var user = { id: 7, nama: "Archana", active: true };
var keys = "";
for (var k in user) { keys = keys + k + " "; }
console.log("keys", keys);
