// core.js — CGen class skeleton + infrastructure methods.
const { C_KEYWORDS, cStr, isNumeric, isObj, promote, isFloatLiteral } = require('./helpers');

class CGen {
  constructor(opts = {}) {
    this.opts = opts;
    this.scopes = [];             // stack of Map(name -> sym)
    this.structs = {};            // signature -> struct name
    this.structDefs = [];         // emitted struct definitions
    this.fnPtrTypedefs = [];      // emitted typedefs
    this.headers = new Set(['stdio.h', 'stdlib.h', 'string.h']);
    this.needMath = false;
    this.tmp = 0;                 // temp counter
    this.structCounter = 0;
    this.fnCounter = 0;
    this.fnPtrCounter = 0;
    this.pre = [];                // pending pre-lines
    this.ind = 1;
    this.lines = [];
    this.warnings = [];
  }

  /* ---------- scope / symbols ---------- */
  pushScope() { this.scopes.push(new Map()); }
  popScope() { this.scopes.pop(); }
  declare(name, sym) {
    const top = this.scopes[this.scopes.length - 1];
    top.set(name, sym);
  }
  lookup(name) {
    for (let i = this.scopes.length - 1; i >= 0; i--) {
      const s = this.scopes[i];
      if (s.has(name)) return s.get(name);
    }
    return null;
  }
  cName(name) {
    // sanitize JS identifier into a valid C identifier
    let out = '';
    for (let i = 0; i < name.length; i++) {
      const c = name[i];
      const ok = (i === 0)
        ? /[A-Za-z_]/.test(c)
        : /[A-Za-z0-9_]/.test(c);
      out += ok ? c : '_';
    }
    if (C_KEYWORDS.has(out)) out = 'm_' + out;
    return out;
  }

  /* ---------- line helpers ---------- */
  line(s) { this.lines.push('    '.repeat(this.ind) + s); }
  flushPre() {
    for (const p of this.pre) this.line(p);
    this.pre = [];
  }
  temp(base) { return '__' + base + '_' + (this.tmp++); }

  /* ---------- type coercion ---------- */
  toNum(code, type) {
    if (type === 'int' || type === 'double') return code;
    if (type === 'mochi_val') return `mochi_val_num(${code})`;
    if (type === 'null') return '0';
    return `((double)(${code}))`;
  }
  toInt(code, type) {
    if (type === 'int') return code;
    if (type === 'double') return `((int)(${code}))`;
    if (type === 'mochi_val') return `((int)mochi_val_num(${code}))`;
    return `((int)(${code}))`;
  }
  toStr(code, type) {
    if (type === 'string') return code;
    if (type === 'int' || type === 'double') return `mochi_num_to_str(${this.toNum(code, type)})`;
    if (type === 'bool') return `((${code}) ? "true" : "false")`;
    if (type === 'mochi_val') return `mochi_val_str(${code})`;
    if (type === 'null') return '"null"';
    return code;
  }
  toBool(code, type) {
    if (type === 'int' || type === 'double') return `((${code}) != 0)`;
    if (type === 'string') return `((${code})[0] != 0)`;
    if (type === 'mochi_val') return `mochi_val_bool(${code})`;
    if (type === 'arr') return `((${code})->len > 0)`;
    return `((${code}) != 0)`;
  }

  /* coerce an expression of `type` to `target` */
  coerce(code, type, target) {
    if (type === target) return code;
    if (isObj(type) && isObj(target) && type === target) return code;
    if (target === 'double') return this.toNum(code, type);
    if (target === 'int') return this.toInt(code, type);
    if (target === 'string') return this.toStr(code, type);
    if (target === 'bool') return this.toBool(code, type);
    if (target === 'mochi_val') return this.box(code, type);
    return code;
  }
  box(code, type) {
    if (type === 'mochi_val') return code;
    if (type === 'int' || type === 'double') return `mochi_num(${this.toNum(code, type)})`;
    if (type === 'string') return `mochi_str(${code})`;
    if (type === 'bool') return `mochi_bool(${code})`;
    if (type === 'null') return 'mochi_null()';
    if (type === 'arr') return `mochi_arr_box(${code})`;
    if (isObj(type)) return `mochi_obj_box(${code})`;
    return code;
  }

  /* ---------- entry ---------- */
  cTypeName(type) {
    switch (type) {
      case 'int': case 'double': case 'string': return type === 'string' ? 'const char *' : type;
      case 'bool': return 'int';
      case 'null': return 'void *';
      case 'arr': return 'mochi_array *';
      case 'mochi_val': return 'mochi_val';
      case 'void': return 'void';
      default:
        if (isObj(type)) return 'struct ' + type.slice(4) + ' *';
        return 'double';
    }
  }

  /* ---------- expressions ---------- */
  generate(program) {
    const prog = program.type === 'Program' ? program : program;
    // hoist function declarations so recursion & forward refs work
    const hoisted = new Map();
    for (const st of prog.body) {
      if (st.type === 'FunctionDeclaration') {
        hoisted.set(st.id.name, st);
      }
    }
    this.pushScope();
    for (const [name, fn] of hoisted) {
      const cname = this.cName(name);
      const sym = this.analyzeFunction(fn, cname);
      this.declare(name, sym);
    }

    // emit helper funcs (printers) + structs + user functions
    const fnLines = [];
    for (const [name, fn] of hoisted) {
      fnLines.push(...this.emitFunctionDecl(fn));
    }

    // top-level body -> main()
    const mainBody = [];
    for (const st of prog.body) {
      if (st.type !== 'FunctionDeclaration') {
        mainBody.push(...this.emitStmt(st));
      }
    }
    if (mainBody.length === 0) mainBody.push('    (void)0;');

    const head = this.renderPreamble();
    let out = head;
    if (this.structDefs.length) out += '\n' + this.structDefs.join('\n');
    if (this.fnPtrTypedefs.length) out += '\n' + this.fnPtrTypedefs.join('\n');
    if (fnLines.length) out += '\n' + fnLines.join('\n');
    out += '\nint main(void) {\n' + mainBody.join('\n') + '\n    return 0;\n}\n';
    this.popScope();
    return { code: out, warnings: this.warnings };
  }
  renderPreamble() {
    const h = [...this.headers];
    let out = '/* generated by cmochicode (JS -> C) — do not edit by hand */\n';
    for (const x of h) out += '#include <' + x + '>\n';
    if (this.needMath) out += '#include <math.h>\n';
    out += '#include "mochi.h"\n';
    return out;
  }

  /* ---------- function analysis ---------- */
}

module.exports = { CGen };
