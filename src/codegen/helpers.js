// helpers.js — shared free functions + constants for the CGen oracle.
'use strict';
/*
 * cmochicode codegen — JavaScript AST (ESTree/acorn) -> C source.
 *
 * Design goals (see README):
 *   1. Emit readable, mostly-plain C. Runtime is called only when the
 *      program must borrow the memory allocator (GC) or do a heap/string
 *      op; everything else is puts/printf/arith/structs.
 *   2. console.log("x")            -> puts("x");            (shortest)
 *      console.log("n =", n)       -> printf("n = %d\n", n); (formatted)
 *   3. Objects become C structs (allocated from the GC, never malloc).
 *      Arrays become mochi_array (GC-backed, heterogeneous via mochi_val).
 *   4. No bytecode, no interpreter loop — this is source -> source.
 */

const C_KEYWORDS = new Set([
  'auto','break','case','char','const','continue','default','do','double',
  'else','enum','extern','float','for','goto','if','inline','int','long',
  'register','restrict','return','short','signed','sizeof','static','struct',
  'switch','typedef','union','unsigned','void','volatile','while',
  '_Bool','_Complex','_Imaginary','_Alignas','_Alignof','_Atomic','_Generic',
  '_Noreturn','_Static_assert','_Thread_local','asm','bool','true','false',
  'NULL','size_t','int8_t','int16_t','int32_t','int64_t','uint8_t',
  'uint16_t','uint32_t','uint64_t',
]);

function cStr(s) {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c === 34) out += '\\"';
    else if (c === 92) out += '\\\\';
    else if (c === 10) out += '\\n';
    else if (c === 13) out += '\\r';
    else if (c === 9) out += '\\t';
    else if (c < 32 || c > 126) out += '\\x' + c.toString(16).padStart(2, '0');
    else out += s[i];
  }
  return out + '"';
}

const isNumeric = (t) => t === 'int' || t === 'double';
const isObj = (t) => typeof t === 'string' && t.startsWith('obj:');

function promote(a, b) {
  if (a === 'string' || b === 'string') return 'string';
  if (a === 'mochi_val' || b === 'mochi_val') return 'mochi_val';
  if (isNumeric(a) && isNumeric(b)) return (a === 'double' || b === 'double') ? 'double' : 'int';
  if (a === 'null') return b;
  if (b === 'null') return a;
  if (isObj(a) && isObj(b) && a === b) return a;
  if (a === 'arr' || b === 'arr') return 'arr';
  if (a === 'void') return b;
  if (b === 'void') return a;
  return 'unknown';
}

function isFloatLiteral(node) {
  return node && node.type === 'Literal' && typeof node.value === 'number' && !Number.isInteger(node.value);
}

module.exports = { C_KEYWORDS, cStr, isNumeric, isObj, promote, isFloatLiteral };
