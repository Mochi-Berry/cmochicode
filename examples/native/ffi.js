const add = ffi("build/demoffi.dll", "add", "i32(i32,i32)");
const version = ffi("build/demoffi.dll", "version", "ptr()");
const scale = ffi("build/demoffi.dll", "scale", "f64(f64,f64)");

console.log("add", add(3, 4));
console.log("version", version());
console.log("scale", scale(1.5, 2));
console.log("once", callOnce("build/demoffi.dll", "add", "i32(i32,i32)", 10, 20));
console.log("counter", ffiGet("build/demoffi.dll", "g_counter", "i32"));
ffiSet("build/demoffi.dll", "g_counter", "i32", 99);
console.log("counter2", ffiGet("build/demoffi.dll", "g_counter", "i32"));
