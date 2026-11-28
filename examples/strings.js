// strings.js — string ops + template literals
var name = "Berry";
var greeting = "Hello " + name + "!";
console.log(greeting);
console.log("len", greeting.length);
console.log("upper", "mochi".toUpperCase());
console.log("lower", "BERRY".toLowerCase());
console.log("idx", greeting.indexOf("Berry"));
console.log("sub", "hello world".substring(0, 5));
console.log("char", "abc".charAt(1));
console.log("tpl", `value=${name}, len=${greeting.length}`);
console.log("trim", "  padded  ".trim());
