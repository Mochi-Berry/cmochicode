// classes.js — class support: constructor, methods, `this`, `new`.
class Counter {
  constructor(start) { this.n = start; this.label = "c" + start; }
  inc() { this.n = this.n + 1; return this.n; }
  plus(k) { return this.n + k; }
  name() { return this.label; }
}
var c = new Counter(10);
console.log("c", c.n, c.name());
console.log("inc", c.inc(), c.inc());
console.log("plus", c.plus(5));
var c2 = new Counter(1);
console.log("c2", c2.n, c2.name(), c2.plus(2));
class Greeter {
  constructor(who) { this.who = who; }
  hello() { return "Hello, " + this.who + "!"; }
}
var g = new Greeter("Berry");
console.log(g.hello());
class Box {
  constructor(v) { this.v = v; }
  get() { return this.v; }
  set(v) { this.v = v; return this; }
}
var b = new Box(3);
console.log("box", b.get(), b.set(9).get());
