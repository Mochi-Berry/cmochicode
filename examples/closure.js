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
