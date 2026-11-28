// control.js — loops, branches, switch, ternary
var total = 0;
for (var i = 0; i < 5; i++) {
  total = total + i;
}
console.log("for sum", total);

var n = 10;
var cnt = 0;
while (n > 0) {
  n = n - 1;
  if (n === 5) continue;
  if (n === 1) break;
  cnt = cnt + 1;
}
console.log("while cnt", cnt);

var day = 2;
var label;
switch (day) {
  case 1: label = "Monday"; break;
  case 2: label = "Tuesday"; break;
  default: label = "other";
}
console.log("day", label);

var grade = 82;
console.log("grade", grade >= 90 ? "A" : grade >= 80 ? "B" : "C");
