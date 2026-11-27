#!/usr/bin/env node
'use strict';
/*
 * cmochicode — JavaScript -> C source transpiler.
 *
 *   node src/index.js in.js [-o out.c] [--stdout] [--lib]
 *
 * Reads JavaScript, produces C89/C99 source. Generated C calls the runtime
 * only to borrow the memory allocator (GC) or for heap/string ops; console
 * output maps to puts()/printf(). No bytecode, no interpreter loop.
 */
const fs = require('fs');
const path = require('path');
const acorn = require('acorn');
const { CGen } = require('./codegen');

function usage() {
  console.error('usage: cmochicode <input.js> [-o output.c] [--stdout] [--lib]');
}

function main(argv) {
  const args = [...argv];
  let input = null, output = null, toStdout = false, lib = false;
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '-o' || a === '--output') { output = args[++i]; }
    else if (a === '--stdout') toStdout = true;
    else if (a === '--lib') lib = true;
    else if (a === '-h' || a === '--help') { usage(); return 0; }
    else if (!input) input = a;
    else { usage(); return 1; }
  }
  if (!input) { usage(); return 1; }

  const src = fs.readFileSync(input, 'utf8');
  let ast;
  try {
    ast = acorn.parse(src, {
      ecmaVersion: 'latest',
      sourceType: 'script',
      allowReturnOutsideFunction: false,
      locations: true,
    });
  } catch (e) {
    console.error(`parse error: ${e.message}`);
    return 2;
  }

  const gen = new CGen({ lib });
  let result;
  try {
    result = gen.generate(ast);
  } catch (e) {
    console.error(`codegen error: ${e.message}`);
    if (process.env.CMOCHI_DEBUG) console.error(e.stack);
    return 3;
  }

  for (const w of result.warnings) {
    console.error('warning: ' + w);
  }

  let code = result.code;
  if (lib) {
    // strip the main() wrapper for library emission
    code = code.replace(/\nint main\(void\) \{[\s\S]*?\n\}\s*$/, '\n');
  }
  if (toStdout || !output) {
    process.stdout.write(code);
  } else {
    const dir = path.dirname(path.resolve(output));
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(output, code);
    console.error(`wrote ${path.resolve(output)} (${code.length} bytes)`);
  }
  return 0;
}

if (require.main === module) {
  process.exit(main(process.argv.slice(2)));
}
module.exports = { main };
