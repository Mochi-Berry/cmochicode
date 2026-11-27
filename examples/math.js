// math.js — arithmetic, functions, branches
function add(a, b) {
  return a + b;
}

function fact(n) {
  var r = 1;
  while (n > 1) {
    r = r * n;
    n = n - 1;
  }
  return r;
}

console.log(add(2, 3));
console.log("fact(5) =", fact(5));
console.log("avg", (1 + 2 + 3) / 3);
console.log("mod", 17 % 5);
console.log("pow", Math.pow(2, 10));
console.log("abs", Math.abs(-42));
console.log("max", Math.max(3, 9, 2));
console.log("floor", Math.floor(3.7));
console.log("int div", 7 / 2);
if (fact(3) === 6) {
  console.log("3! ok");
} else {
  console.log("3! wrong");
}
