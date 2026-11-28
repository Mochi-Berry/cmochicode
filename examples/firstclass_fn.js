/* First-class functions: boxed callables, call-through-value, function.length. */
function add(a, b) { return a + b; }
console.log("direct", add(2, 3));

var f = add;                       /* function as a value */
console.log("via var", f(4, 5));
console.log("fnlen", add.length);  /* params before default/rest */

var arr = [];
arr.push(add);                     /* function in an array */
console.log("via arr", arr[0](6, 7));
console.log("print", add);         /* [Function: add] */

function make(n) {
    return function () { return n + 1; };
}
var c = make(10);
console.log("maker", c());         /* no local capture: n is the param */

var twice = (x) => x * 2;          /* arrow as a value */
console.log("arrow", twice(21));
console.log("arrow boxed", twice);

function apply(fn, v) { return fn(v); }
console.log("higher", apply(twice, 5), apply(make_adder(100), 12));

/* the returned closure keeps its captured cell alive via the box's env */
function make_adder(k) {
    return function (x) { return x + k; };
}