const n = 10000000;
const t0 = Date.now();
let s = 0;
for (let i = 0; i < n; i++) s = s + 1;
const t1 = Date.now();
console.log("direct_ms", t1 - t0, "sum", s);
