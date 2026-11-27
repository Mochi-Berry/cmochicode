// fib.js — recursion
function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
for (var i = 0; i < 10; i++) {
  console.log("fib", i, fib(i));
}
