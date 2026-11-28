function add(a, b) { return a + b; }

/* a missing argument is `undefined`, and undefined is NaN in arithmetic */
console.log("add3", add(3));
var one = [3];
console.log("spread1", add(...one));
console.log("undef", add(3, undefined));

/* null coerces to 0 in arithmetic (JS: 3 + null === 3) */
console.log("null", add(3, null));

/* how the two bottom values print */
console.log("undef-print", undefined);
console.log("null-print", null);
console.log("void0", void 0);

/* falsy? both bottom values are falsy */
console.log("falsy", undefined ? "t" : "f", null ? "t" : "f");

/* identity: null and undefined are different bottom values */
console.log("cmp", null === null, undefined === null, null === undefined);
