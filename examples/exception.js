// exception.js — real exception unwinding (setjmp/longjmp).
// A throw unwinds the C stack to the innermost live try frame; the try body
// is abandoned, the catch runs, and execution continues after the try.
function level3() { throw "l3"; }
function level2() { level3(); }
function level1() { level2(); }

// unwind through three call frames
try {
    level1();
    console.log("not reached");
} catch (e) {
    console.log("through-3-frames", e);
}

// throw from inside a loop: the catch body runs, the loop continues
function loopThrow() {
    var total = 0;
    for (var i = 0; i < 4; i++) {
        try { if (i === 2) { throw "loop" + i; } total += i; } catch (e) { total += 100; }
    }
    return total;
}
console.log("loop", loopThrow());

// return from inside a try block (the frame is popped before returning)
function withRet(x) {
    try { if (x) { return "ret-from-try"; } return "ret-fallthrough"; }
    catch (e) { return "ret-from-catch"; }
}
console.log(withRet(true));

// nested try: inner catch rethrows, outer catch sees it, and a local written
// in the inner catch survives the unwind (volatile under the C setjmp rule)
var seen = 0;
try {
    try { throw "nested"; }
    catch (e) { seen = 1; console.log("inner", e); throw e; }
} catch (e2) {
    console.log("outer", e2, "inner-ran", seen);
}

// a try that never throws still runs its body
var u = 0;
try { u = u + 1; } catch (e) { }
console.log("no-throw", u);
