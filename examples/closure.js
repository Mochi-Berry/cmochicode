/* Closure capture: upvalue cells, mutable captures, nested closures. */
function make_counter() {
    let count = 0;
    return function () { count = count + 1; return count; };
}
var c = make_counter();
console.log("counter", c(), c(), c());

var c2 = make_counter();
console.log("independent", c2(), c2());

function make_adder(k) {
    return function (x) { return x + k; };
}
console.log("adder", make_adder(100)(12), make_adder(5)(1));

function outer(y) {
    return function (z) {
        return function (w) { return y + z + w; };
    };
}
console.log("nested", outer(1)(2)(3));

var label = "global";
var gl = () => label + "!";
console.log("global", gl());

/* `var` in a for-head is ONE binding: every closure sees the final value */
var fns = [];
for (var i = 0; i < 3; i++) { fns.push(function (x) { return x + i; }); }
console.log("var-loop", fns[0](10), fns[1](10), fns[2](10));

/* `let` gets a fresh binding per iteration, so each closure keeps its own */
var lfns = [];
for (let j = 0; j < 3; j++) { lfns.push(function (x) { return x + j; }); }
console.log("let-loop", lfns[0](10), lfns[1](10), lfns[2](10));
