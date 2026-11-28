// objects.js — object literals become C structs (GC-allocated)
var point = { x: 3, y: 4 };
point.x = point.x + 1;
point.label = "P";
console.log(point);
console.log("x", point.x, "y", point.y);

var person = { name: "Nonamey", age: 30, active: true };
console.log(person);

var { x, y } = point;
console.log("destructured", x, y);
