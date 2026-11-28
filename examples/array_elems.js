// array_elems.js — typed array elements: the callback parameter keeps the
// element type of the receiver (string arrays stay strings, int arrays stay
// ints), and the printed value must match node exactly.
var words = ["a", "b", "c"];
console.log("str-map", words.map(w => w + "!"));
console.log("str-join", words.join("-"));
console.log("str-each", words.length);

var nums = [1, 2, 3];
console.log("int-map", nums.map(n => n * 3));
console.log("int-sum", nums.reduce((a, n) => a + n, 0));
console.log("int-big", nums.map(n => n * 1000000));

var mixed = [1.5, 2.5];
console.log("dbl-map", mixed.map(d => d + 0.5));

// the element type survives a variable, not just a literal
var alias = words;
console.log("alias-map", alias.map(w => w + "?"));
console.log("alias-filter", nums.filter(n => n > 1));
console.log("alias-some", nums.some(n => n == 2));
