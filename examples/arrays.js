// arrays.js — heterogeneous arrays via mochi_array
var a = [];
a.push(1);
a.push(2);
a.push("three");
console.log("len", a.length);
console.log("join", a.join(", "));
console.log("elem1", a[1]);

var b = [10, 20, 30];
var sum = 0;
for (var v of b) {
  sum = sum + v;
}
console.log("sum", sum);
console.log("b", b);
