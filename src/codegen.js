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
  analyzeFunction(fn, cname) {
    // infer param types + return type from body
    const paramTypes = [];
    const uses = new Map();
    for (const p of fn.params) {
      uses.set(p.name, { double: 0, string: 0, int: 0 });
    }
    const scan = (node, ctx) => {
      if (!node || typeof node !== 'object') return;
      if (Array.isArray(node)) { node.forEach(n => scan(n, ctx)); return; }
      // record param usage
      if (node.type === 'Identifier' && uses.has(node.name)) {
        const u = uses.get(node.name);
        if (ctx === 'double') u.double++;
        else if (ctx === 'string') u.string++;
        else if (ctx === 'int') u.int++;
      }
      if (node.type === 'BinaryExpression') {
        const leftCtx = node.operator === '+' && (node.left.type === 'Literal' && typeof node.left.value === 'string')
          ? 'string'
          : (node.operator === '/' ? 'double' : (isFloatLiteral(node.right) ? 'double' : 'int'));
        const rightCtx = node.operator === '+' && (node.right.type === 'Literal' && typeof node.right.value === 'string')
          ? 'string'
          : (node.operator === '/' ? 'double' : (isFloatLiteral(node.left) ? 'double' : 'int'));
        scan(node.left, leftCtx);
        scan(node.right, rightCtx);
      } else if (node.type === 'MemberExpression') {
        scan(node.object, ctx);
        if (node.computed) scan(node.property, 'int');
      } else if (node.type === 'CallExpression') {
        scan(node.callee, ctx);
        node.arguments.forEach(a => scan(a, null));
      } else if (node.type === 'ReturnStatement') {
        if (node.argument) scan(node.argument, 'double');
      } else if (node.type === 'VariableDeclarator') {
        if (node.init) scan(node.init, null);
      } else if (node.type === 'AssignmentExpression') {
        scan(node.right, node.operator === '+' ? 'string' : 'double');
        scan(node.left, ctx);
      } else {
        for (const k in node) {
          if (k === 'loc' || k === 'start' || k === 'end' || k === 'range') continue;
          scan(node[k], ctx);
        }
      }
    };
    for (const st of fn.body.body) scan(st, null);

    for (const p of fn.params) {
      const u = uses.get(p.name) || { double: 0, string: 0, int: 0 };
      let t = 'double';
      if (u.string > u.double && u.string > u.int) t = 'string';
      else if (u.double === 0 && u.int > 0) t = 'int';
      else if (u.double > 0 && u.int === 0) t = 'double';
      paramTypes.push(t);
    }
    // return type
    let ret = 'void';
    let sawRet = false;
    const collectRet = (node) => {
      if (!node || typeof node !== 'object') return;
      if (Array.isArray(node)) { node.forEach(collectRet); return; }
      if (node.type === 'ReturnStatement' && node.argument) {
        sawRet = true;
        const t = this.inferExprType(node.argument);
        ret = ret === 'void' ? t : promote(ret, t);
      } else {
        for (const k in node) {
          if (k === 'loc' || k === 'start' || k === 'end') continue;
          collectRet(node[k]);
        }
      }
    };
    collectRet(fn.body);
    if (!sawRet) ret = 'void';
    if (ret === 'unknown') ret = 'double';

    return { cname, type: ret, params: fn.params.map((p, i) => ({ name: p.name, type: paramTypes[i] })), node: fn };
  }

  inferExprType(node) {
    if (!node) return 'void';
    switch (node.type) {
      case 'Literal':
        if (node.value === null) return 'null';
        if (typeof node.value === 'number') return Number.isInteger(node.value) ? 'int' : 'double';
        if (typeof node.value === 'string') return 'string';
        if (typeof node.value === 'boolean') return 'int';
        return 'unknown';
      case 'Identifier': {
        const s = this.lookup(node.name);
        return s ? s.type : 'unknown';
      }
      case 'UnaryExpression': {
        if (node.operator === 'typeof') return 'string';
        return this.inferExprType(node.argument);
      }
      case 'BinaryExpression': {
        const l = this.inferExprType(node.left);
        const r = this.inferExprType(node.right);
        switch (node.operator) {
          case '+': return promote(l, r);
          case '/': return 'double';
          case '%': return 'int';
          case '==': case '===': case '!=': case '!==':
          case '<': case '<=': case '>': case '>=': return 'int';
          case '&&': case '||': return promote(l, r);
          default: return (isNumeric(l) && isNumeric(r)) ? (l === 'double' || r === 'double' ? 'double' : 'int') : 'unknown';
        }
      }
      case 'ConditionalExpression': {
        const a = this.inferExprType(node.consequent);
        const b = this.inferExprType(node.alternate);
        return promote(a, b);
      }
      case 'CallExpression': return this.inferCallType(node);
      case 'MemberExpression': return this.inferMemberType(node);
      case 'ArrayExpression': return 'arr';
      case 'ObjectExpression': return 'unknown';
      case 'AssignmentExpression': return this.inferExprType(node.right);
      case 'SequenceExpression': return this.inferExprType(node.expressions[node.expressions.length - 1]);
      case 'TemplateLiteral': return 'string';
      case 'NewExpression': return 'unknown';
      case 'ThisExpression': return 'unknown';
      default: return 'unknown';
    }
  }

  inferCallType(node) {
    const callee = node.callee;
    if (callee.type === 'Identifier') {
      const name = callee.name;
      if (name === 'parseInt' || name === 'parseFloat' || name === 'Number') return 'double';
      if (name === 'String') return 'string';
      if (name === 'Boolean' || name === 'isNaN') return 'int';
      const s = this.lookup(name);
      if (s && s.type && s.type !== 'unknown') return s.type;
      return 'unknown';
    }
    if (callee.type === 'MemberExpression') {
      const objType = this.inferExprType(callee.object);
      const prop = callee.computed ? null : callee.property.name;
      if (objType === 'arr') {
        if (prop === 'push' || prop === 'pop' || prop === 'join' || prop === 'length') return 'int';
      }
      if (objType === 'string') {
        if (['toUpperCase','toLowerCase','charAt','substring','trim','slice','replace'].includes(prop)) return 'string';
        if (['indexOf','includes','endsWith','startsWith'].includes(prop)) return 'int';
      }
      return 'unknown';
    }
    return 'unknown';
  }

  inferMemberType(node) {
    const objType = this.inferExprType(node.object);
    if (node.computed) {
      if (objType === 'arr') return 'mochi_val';
      return 'unknown';
    }
    const prop = node.property.name;
    if (objType === 'arr' && prop === 'length') return 'int';
    if (objType === 'string' && prop === 'length') return 'int';
    if (isObj(objType)) {
      // look up field in the struct def we recorded
      const fields = this.structFields[objType];
      if (fields && fields[prop]) return fields[prop].type;
      return 'unknown';
    }
    if (objType === 'string') return 'unknown';
    return 'unknown';
  }

  /* ---------- statements ---------- */
  emitStmt(node) {
    this.flushPre();
    switch (node.type) {
      case 'ExpressionStatement': return this.emitExprStatement(node);
      case 'VariableDeclaration': return this.emitVarDecl(node);
      case 'FunctionDeclaration': return [];
      case 'ReturnStatement': {
        if (node.argument) {
          const e = this.emitExpr(node.argument);
          return ['    '.repeat(this.ind) + 'return ' + e.code + ';'];
        }
        return ['    '.repeat(this.ind) + 'return;'];
      }
      case 'IfStatement': return this.emitIf(node);
      case 'WhileStatement': {
        const cond = this.emitExpr(node.test);
        const body = this.emitBlockStmts(node.body);
        return ['    '.repeat(this.ind) + 'while (' + this.toBool(cond.code, cond.type) + ') {', ...body, '    '.repeat(this.ind) + '}'];
      }
      case 'DoWhileStatement': {
        const body = this.emitBlockStmts(node.body);
        const cond = this.emitExpr(node.test);
        return ['    '.repeat(this.ind) + 'do {', ...body, '    '.repeat(this.ind) + '} while (' + this.toBool(cond.code, cond.type) + ');'];
      }
      case 'ForStatement': return this.emitFor(node);
      case 'ForOfStatement': return this.emitForOf(node);
      case 'BreakStatement': return ['    '.repeat(this.ind) + 'break;'];
      case 'ContinueStatement': return ['    '.repeat(this.ind) + 'continue;'];
      case 'BlockStatement': return this.emitBlockStmts(node);
      case 'EmptyStatement': return [];
      case 'SwitchStatement': return this.emitSwitch(node);
      case 'TryStatement': return this.emitTry(node);
      case 'ThrowStatement': {
        const e = this.emitExpr(node.argument);
        return ['    '.repeat(this.ind) + 'fprintf(stderr, "Exception: %s\\n", ' + this.toStr(e.code, e.type) + ');'];
      }
      case 'DebuggerStatement': return [];
      default:
        this.warnings.push('unsupported statement: ' + node.type);
        return [];
    }
  }

  emitBlockStmts(node) {
    const body = node.type === 'BlockStatement' ? node.body : [node];
    const out = [];
    this.ind++;
    for (const st of body) {
      const r = this.emitStmt(st);
      for (const l of r) out.push(l);
    }
    this.ind--;
    return out;
  }

  emitExprStatement(node) {
    const e = this.emitExpr(node.expression);
    if (e.pre && e.pre.length) {
      const pre = e.pre;
      const out = [];
      for (const p of pre) out.push('    '.repeat(this.ind) + p);
      return out;
    }
    if (e.type === 'void' || e.type === 'null') return ['    '.repeat(this.ind) + e.code + ';'];
    return ['    '.repeat(this.ind) + '(void)(' + e.code + ');'];
  }

  emitIf(node) {
    const cond = this.emitExpr(node.test);
    const out = [];
    out.push('    '.repeat(this.ind) + 'if (' + this.toBool(cond.code, cond.type) + ') {');
    const thenB = this.emitBlockStmts(node.consequent);
    out.push(...thenB);
    if (node.alternate) {
      out.push('    '.repeat(this.ind) + '} else {');
      const elseB = this.emitBlockStmts(node.alternate);
      out.push(...elseB);
    }
    out.push('    '.repeat(this.ind) + '}');
    return out;
  }

  emitFor(node) {
    const out = [];
    let init = '';
    if (node.init) {
      if (node.init.type === 'VariableDeclaration') {
        const d = this.emitVarDecl(node.init);
        init = d.join(' ').replace(/\s+/g, ' ').replace(/;\s*$/, ';');
        if (d.length > 1) init = d[0];
      } else {
        const e = this.emitExpr(node.init);
        init = e.code;
      }
    }
    let test = '';
    if (node.test) {
      const t = this.emitExpr(node.test);
      test = this.toBool(t.code, t.type);
    }
    let upd = '';
    if (node.update) {
      const u = this.emitExpr(node.update);
      upd = u.code;
    }
    out.push('    '.repeat(this.ind) + 'for (' + init + '; ' + test + '; ' + upd + ') {');
    const body = this.emitBlockStmts(node.body);
    out.push(...body);
    out.push('    '.repeat(this.ind) + '}');
    return out;
  }

  emitForOf(node) {
    const right = this.emitExpr(node.right);
    if (right.type !== 'arr') {
      this.warnings.push('for-of over non-array; treating as array');
    }
    const arr = right.code;
    const idx = this.temp('i');
    const val = this.temp('v');
    // declare loop variable as a real C variable in this scope
    let loopVar;
    if (node.left.type === 'VariableDeclaration') {
      loopVar = node.left.declarations[0].id.name;
      this.declare(loopVar, { cname: this.cName(loopVar), type: 'mochi_val' });
    } else {
      loopVar = node.left.name;
      this.declare(loopVar, { cname: this.cName(loopVar), type: 'mochi_val' });
    }
    const out = [];
    out.push('    '.repeat(this.ind) + `for (size_t ${idx} = 0; ${idx} < (${arr})->len; ${idx}++) {`);
    this.ind++;
    out.push('    '.repeat(this.ind) + `mochi_val ${val} = (${arr})->items[${idx}];`);
    out.push('    '.repeat(this.ind) + `mochi_val ${this.cName(loopVar)} = ${val};`);
    const body = this.emitBlockStmts(node.body);
    out.push(...body);
    this.ind--;
    out.push('    '.repeat(this.ind) + '}');
    return out;
  }

  emitSwitch(node) {
    const disc = this.emitExpr(node.discriminant);
    const out = [];
    let first = true;
    for (const c of node.cases) {
      let cond;
      if (c.test) {
        const t = this.emitExpr(c.test);
        if (disc.type === 'string' || t.type === 'string') {
          cond = `(strcmp(${disc.code}, ${t.code}) == 0)`;
        } else {
          cond = `(${this.toNum(disc.code, disc.type)} == ${this.toNum(t.code, t.type)})`;
        }
      } else {
        cond = null; // default
      }
      if (cond) {
        out.push('    '.repeat(this.ind) + (first ? 'if (' : 'else if (') + cond + ') {');
      } else {
        out.push('    '.repeat(this.ind) + (first ? '' : '} else ') + '{');
      }
      first = false;
      this.ind++;
      for (const st of c.consequent) {
        if (st.type === 'BreakStatement') continue;
        out.push(...this.emitStmt(st));
      }
      this.ind--;
    }
    if (out.length) out.push('    '.repeat(this.ind) + '}');
    return out;
  }

  emitTry(node) {
    this.warnings.push('try/catch is best-effort: catch emits to stderr, no unwinding');
    const out = [];
    const body = this.emitBlockStmts(node.block);
    out.push(...body);
    return out;
  }

  emitVarDecl(node) {
    const out = [];
    for (const d of node.declarations) {
      if (d.id.type === 'ObjectPattern' || d.id.type === 'ArrayPattern') {
        out.push(...this.emitPattern(d));
        continue;
      }
      const name = d.id.name;
      const cname = this.cName(name);
      let type, init;
      if (d.init) {
        const e = this.emitExpr(d.init);
        type = e.type;
        if (type === 'void' || type === 'null') type = 'int';
        if (type === 'unknown') type = 'double';
        init = e.code;
      } else {
        type = 'double';
        init = '0';
      }
      this.declare(name, { cname, type });
      const t = this.cTypeName(type);
      out.push('    '.repeat(this.ind) + `${t} ${cname} = ${this.coerce(init, type, type)};`);
    }
    return out;
  }

  emitPattern(d) {
    const out = [];
    if (d.id.type === 'ObjectPattern') {
      if (!d.init) return out;
      const e = this.emitExpr(d.init);
      if (isObj(e.type)) {
        for (const prop of d.id.properties) {
          if (prop.type !== 'Property') continue;
          const key = prop.key.name;
          const target = prop.value.name;
          const tname = this.cName(target);
          const ftype = this.structFields[e.type] && this.structFields[e.type][key]
            ? this.structFields[e.type][key].type : 'double';
          this.declare(target, { cname: tname, type: ftype });
          out.push('    '.repeat(this.ind) + `${this.cTypeName(ftype)} ${tname} = ${e.code}->${key};`);
        }
      } else {
        this.warnings.push('object destructuring from non-object');
      }
    } else if (d.id.type === 'ArrayPattern') {
      if (!d.init) return out;
      const e = this.emitExpr(d.init);
      const arr = this.temp('arr');
      out.push('    '.repeat(this.ind) + `mochi_array *${arr} = ${e.code};`);
      d.id.elements.forEach((el, i) => {
        if (!el) return;
        const name = el.name;
        const cname = this.cName(name);
        this.declare(name, { cname, type: 'mochi_val' });
        out.push('    '.repeat(this.ind) + `mochi_val ${cname} = mochi_array_get(${arr}, ${i});`);
      });
    }
    return out;
  }

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
  emitExpr(node) {
    switch (node.type) {
      case 'Literal': return this.emitLiteral(node);
      case 'Identifier': return this.emitIdentifier(node);
      case 'UnaryExpression': return this.emitUnary(node);
      case 'BinaryExpression': return this.emitBinary(node);
      case 'LogicalExpression': return this.emitLogical(node);
      case 'AssignmentExpression': return this.emitAssign(node);
      case 'UpdateExpression': return this.emitUpdate(node);
      case 'CallExpression': return this.emitCall(node);
      case 'MemberExpression': return this.emitMember(node);
      case 'ConditionalExpression': return this.emitConditional(node);
      case 'ArrayExpression': return this.emitArray(node);
      case 'ObjectExpression': return this.emitObject(node);
      case 'SequenceExpression': {
        let last = { code: '0', type: 'int', pre: [] };
        for (const e of node.expressions) last = this.emitExpr(e);
        return last;
      }
      case 'TemplateLiteral': return this.emitTemplate(node);
      case 'FunctionExpression': return this.emitFunctionExpr(node);
      case 'ArrowFunctionExpression': return this.emitFunctionExpr(node);
      case 'ThisExpression': {
        this.warnings.push('`this` not supported; emitted 0');
        return { code: '0', type: 'int', pre: [] };
      }
      case 'NewExpression': return this.emitNew(node);
      case 'AwaitExpression': {
        this.warnings.push('await not supported');
        return this.emitExpr(node.argument);
      }
      case 'ParenthesizedExpression': return this.emitExpr(node.expression);
      default:
        this.warnings.push('unsupported expression: ' + node.type);
        return { code: '0', type: 'int', pre: [] };
    }
  }

  emitLiteral(node) {
    const v = node.value;
    if (v === null) return { code: '0', type: 'null', pre: [] };
    if (typeof v === 'number') {
      if (Number.isInteger(v) && Math.abs(v) <= 0x7fffffff) return { code: String(v), type: 'int', pre: [] };
      if (Number.isInteger(v) && Math.abs(v) <= 0x1fffffffffffff) return { code: String(v) + '.0', type: 'double', pre: [] };
      return { code: String(v), type: 'double', pre: [] };
    }
    if (typeof v === 'string') return { code: cStr(v), type: 'string', pre: [] };
    if (typeof v === 'boolean') return { code: v ? '1' : '0', type: 'int', pre: [] };
    return { code: '0', type: 'null', pre: [] };
  }

  emitIdentifier(node) {
    const s = this.lookup(node.name);
    if (s) return { code: s.cname, type: s.type, pre: [] };
    // builtin globals used as values
    if (node.name === 'Math' || node.name === 'console' || node.name === 'JSON' || node.name === 'undefined') {
      return { code: '0', type: node.name === 'undefined' ? 'null' : 'unknown', pre: [] };
    }
    if (node.name === 'NaN' || node.name === 'Infinity') {
      return { code: node.name === 'NaN' ? 'NAN' : 'INFINITY', type: 'double', pre: [] };
    }
    this.warnings.push('unknown identifier: ' + node.name + ' (emitted as-is; C compiler will catch it)');
    return { code: this.cName(node.name), type: 'unknown', pre: [] };
  }

  emitUnary(node) {
    if (node.operator === 'typeof') {
      const a = this.emitExpr(node.argument);
      let t = 'number';
      if (a.type === 'string') t = 'string';
      else if (a.type === 'null') t = 'object';
      else if (a.type === 'arr') t = 'object';
      else if (isObj(a.type)) t = 'object';
      else if (a.type === 'unknown') t = 'undefined';
      return { code: cStr(t), type: 'string', pre: a.pre || [] };
    }
    const a = this.emitExpr(node.argument);
    const pre = a.pre || [];
    switch (node.operator) {
      case '-': return { code: `(-(${this.toNum(a.code, a.type)}))`, type: isNumeric(a.type) ? a.type : 'double', pre };
      case '+': return { code: `(+(${this.toNum(a.code, a.type)}))`, type: isNumeric(a.type) ? a.type : 'double', pre };
      case '!': return { code: `(!(${this.toBool(a.code, a.type)}))`, type: 'int', pre };
      case '~': return { code: `(~(${this.toInt(a.code, a.type)}))`, type: 'int', pre };
      default: return { code: a.code, type: a.type, pre };
    }
  }

  emitBinary(node) {
    const l = this.emitExpr(node.left);
    const r = this.emitExpr(node.right);
    const pre = [...(l.pre || []), ...(r.pre || [])];
    const op = node.operator;
    // string concat
    if (op === '+' && (l.type === 'string' || r.type === 'string')) {
      this.headers.add('string.h');
      const ls = this.toStr(l.code, l.type);
      const rs = this.toStr(r.code, r.type);
      return { code: `mochi_concat(${ls}, ${rs})`, type: 'string', pre };
    }
    // numeric / comparison
    const bothStr = l.type === 'string' && r.type === 'string';
    switch (op) {
      case '+': return { code: `(${this.toNum(l.code, l.type)} + ${this.toNum(r.code, r.type)})`, type: promote(l.type, r.type) === 'string' ? 'double' : promote(l.type, r.type), pre };
      case '-': return { code: `(${this.toNum(l.code, l.type)} - ${this.toNum(r.code, r.type)})`, type: 'double', pre };
      case '*': return { code: `(${this.toNum(l.code, l.type)} * ${this.toNum(r.code, r.type)})`, type: 'double', pre };
      case '/': this.needMath = true; return { code: `(${this.toNum(l.code, l.type)} / ${this.toNum(r.code, r.type)})`, type: 'double', pre };
      case '%': {
        if (l.type === 'int' && r.type === 'int') return { code: `(${l.code} % ${r.code})`, type: 'int', pre };
        this.needMath = true;
        return { code: `fmod(${this.toNum(l.code, l.type)}, ${this.toNum(r.code, r.type)})`, type: 'double', pre };
      }
      case '**': this.needMath = true; return { code: `pow(${this.toNum(l.code, l.type)}, ${this.toNum(r.code, r.type)})`, type: 'double', pre };
      case '<': case '<=': case '>': case '>=': {
        if (bothStr) {
          this.headers.add('string.h');
          return { code: `(strcmp(${l.code}, ${r.code}) ${op} 0)`, type: 'int', pre };
        }
        return { code: `(${this.toNum(l.code, l.type)} ${op} ${this.toNum(r.code, r.type)})`, type: 'int', pre };
      }
      case '==': case '===': case '!=': case '!==': {
        if (bothStr) {
          this.headers.add('string.h');
          const cmp = `(strcmp(${l.code}, ${r.code}) == 0)`;
          return { code: op.startsWith('!') ? `(!${cmp})` : cmp, type: 'int', pre };
        }
        const eq = `(${this.toNum(l.code, l.type)} == ${this.toNum(r.code, r.type)})`;
        return { code: op.startsWith('!') ? `(!${eq})` : eq, type: 'int', pre };
      }
      case '&': case '|': case '^':
        return { code: `(${this.toInt(l.code, l.type)} ${op} ${this.toInt(r.code, r.type)})`, type: 'int', pre };
      case '<<': case '>>':
        return { code: `(${this.toInt(l.code, l.type)} ${op} ${this.toInt(r.code, r.type)})`, type: 'int', pre };
      case '>>>':
        return { code: `((unsigned int)${this.toInt(l.code, l.type)} >> ${this.toInt(r.code, r.type)})`, type: 'int', pre };
      default:
        return { code: `(${l.code} ${op} ${r.code})`, type: 'unknown', pre };
    }
  }

  emitLogical(node) {
    const l = this.emitExpr(node.left);
    const r = this.emitExpr(node.right);
    const pre = [...(l.pre || []), ...(r.pre || [])];
    const type = promote(l.type, r.type);
    if (node.operator === '&&') {
      return { code: `((${this.toBool(l.code, l.type)}) ? (${r.code}) : (${l.code}))`, type, pre };
    }
    return { code: `((${this.toBool(l.code, l.type)}) ? (${l.code}) : (${r.code}))`, type, pre };
  }

  emitAssign(node) {
    const r = this.emitExpr(node.right);
    const pre = [...(r.pre || [])];
    if (node.left.type === 'Identifier') {
      const s = this.lookup(node.left.name);
      const target = s ? s.cname : this.cName(node.left.name);
      let type = s ? s.type : r.type;
      if (type === 'unknown') type = r.type === 'unknown' ? 'double' : r.type;
      if (node.operator === '=') {
        const coerced = this.coerce(r.code, r.type, type);
        return { code: `${target} = ${coerced}`, type, pre };
      }
      // compound
      const cop = node.operator.slice(0, -1);
      if (type === 'string' && cop === '+') {
        const ls = this.toStr(target, type);
        const rs = this.toStr(r.code, r.type);
        return { code: `${target} = mochi_concat(${ls}, ${rs})`, type: 'string', pre };
      }
      const t = this.toNum(target, type);
      const rt = this.toNum(r.code, r.type);
      let expr;
      if (cop === '/') expr = `(${t} / ${rt})`;
      else if (cop === '%') expr = `fmod(${t}, ${rt})`;
      else expr = `(${t} ${cop} ${rt})`;
      return { code: `${target} = ${expr}`, type: promote(type, r.type), pre };
    }
    if (node.left.type === 'MemberExpression') {
      return this.emitMemberAssign(node, r, pre);
    }
    return { code: r.code, type: r.type, pre };
  }

  emitUpdate(node) {
    const a = this.emitExpr(node.argument);
    const pre = a.pre || [];
    if (node.argument.type === 'Identifier') {
      const s = this.lookup(node.argument.name);
      const target = s ? s.cname : this.cName(node.argument.name);
      const type = s ? s.type : 'int';
      const op = node.operator === '++' ? '+' : '-';
      if (node.prefix) return { code: `(${target} ${op}= 1)`, type, pre };
      // postfix: preserve old value
      const tmp = this.temp('old');
      pre.push(`${this.cTypeName(type)} ${tmp} = ${target};`);
      pre.push(`${target} ${op}= 1;`);
      return { code: tmp, type, pre };
    }
    return { code: a.code, type: a.type, pre };
  }

  emitConditional(node) {
    const c = this.emitExpr(node.test);
    const a = this.emitExpr(node.consequent);
    const b = this.emitExpr(node.alternate);
    const pre = [...(c.pre || []), ...(a.pre || []), ...(b.pre || [])];
    const type = promote(a.type, b.type);
    return {
      code: `(${this.toBool(c.code, c.type)} ? ${this.coerce(a.code, a.type, type)} : ${this.coerce(b.code, b.type, type)})`,
      type, pre,
    };
  }

  emitMember(node) {
    const obj = this.emitExpr(node.object);
    const pre = [...(obj.pre || [])];
    if (node.computed) {
      const idx = this.emitExpr(node.property);
      pre.push(...(idx.pre || []));
      const o = obj.code;
      if (obj.type === 'arr') {
        return { code: `mochi_array_get(${o}, ${this.toInt(idx.code, idx.type)})`, type: 'mochi_val', pre };
      }
      if (obj.type === 'string') {
        this.headers.add('string.h');
        return { code: `mochi_char_at(${o}, ${this.toInt(idx.code, idx.type)})`, type: 'string', pre };
      }
      // generic object computed access -> unknown
      this.warnings.push('computed member access on non-array/string');
      return { code: `${o}->__get__`, type: 'unknown', pre };
    }
    const prop = node.property.name;
    const o = obj.code;
    if (obj.type === 'arr' && prop === 'length') return { code: `((int)((${o})->len))`, type: 'int', pre };
    if (obj.type === 'string' && prop === 'length') { this.headers.add('string.h'); return { code: `((int)strlen(${o}))`, type: 'int', pre }; }
    if (isObj(obj.type)) {
      const fields = this.structFields[obj.type];
      if (fields && fields[prop]) return { code: `${o}->${prop}`, type: fields[prop].type, pre };
      this.warnings.push('unknown field: ' + prop + ' on ' + obj.type);
      return { code: `${o}->${prop}`, type: 'unknown', pre };
    }
    if (obj.type === 'string') {
      // string method access without call -> treat as unknown value
      return { code: `${o}`, type: 'unknown', pre };
    }
    return { code: `${o}->${prop}`, type: 'unknown', pre };
  }

  emitMemberAssign(node, rhs, pre) {
    const obj = this.emitExpr(node.left.object);
    pre.push(...(obj.pre || []));
    const o = obj.code;
    if (node.left.computed) {
      const idx = this.emitExpr(node.left.property);
      pre.push(...(idx.pre || []));
      const val = this.box(rhs.code, rhs.type);
      const set = `mochi_array_set(${o}, ${this.toInt(idx.code, idx.type)}, ${val})`;
      return { code: set, type: 'mochi_val', pre };
    }
    const prop = node.left.property.name;
    if (isObj(obj.type)) {
      const fields = this.structFields[obj.type];
      const ftype = fields && fields[prop] ? fields[prop].type : rhs.type;
      const coerced = this.coerce(rhs.code, rhs.type, ftype);
      return { code: `${o}->${prop} = ${coerced}`, type: ftype, pre };
    }
    if (obj.type === 'arr' && prop === 'push') {
      const val = this.box(rhs.code, rhs.type);
      return { code: `mochi_array_push(${o}, ${val})`, type: 'int', pre };
    }
    return { code: `${o}->${prop} = ${rhs.code}`, type: rhs.type, pre };
  }

  emitArray(node) {
    const arr = this.temp('arr');
    const pre = [`mochi_array *${arr} = mochi_array_new();`];
    for (const el of node.elements) {
      if (!el) continue;
      const e = this.emitExpr(el);
      pre.push(...(e.pre || []));
      const val = this.box(e.code, e.type);
      pre.push(`mochi_array_push(${arr}, ${val});`);
    }
    return { code: arr, type: 'arr', pre };
  }

  emitObject(node) {
    // determine struct name (dedupe by shape signature)
    const fields = [];
    for (const prop of node.properties) {
      if (prop.type !== 'Property') {
        this.warnings.push('object spread not supported');
        continue;
      }
      const key = prop.key.name;
      const e = this.emitExpr(prop.value);
      const t = e.type === 'unknown' ? 'double' : e.type;
      fields.push({ key, type: t, e });
    }
    const sig = fields.map(f => f.key + ':' + f.type).join(',');
    let structName = this.structs[sig];
    if (!structName) {
      structName = 'obj_' + (this.structCounter++);
      this.structs[sig] = structName;
      const def = ['struct ' + structName + ' {'];
      for (const f of fields) def.push('    ' + this.cTypeName(f.type) + ' ' + f.key + ';');
      def.push('};');
      this.structDefs.push(def.join('\n'));
      this.structFields = this.structFields || {};
      this.structFields[structName] = {};
      for (const f of fields) this.structFields[structName][f.key] = { type: f.type };
    }
    const tmp = this.temp('obj');
    const pre = [`struct ${structName} *${tmp} = (struct ${structName} *)mochi_alloc(sizeof(struct ${structName}));`];
    const curFields = this.structFields[structName];
    for (const f of fields) {
      const coerced = this.coerce(f.e.code, f.e.type, curFields[f.key].type);
      pre.push(`${tmp}->${f.key} = ${coerced};`);
    }
    return { code: tmp, type: 'obj:' + structName, pre };
  }

  emitTemplate(node) {
    const parts = [];
    let cur = '';
    for (let i = 0; i < node.quasis.length; i++) {
      cur += node.quasis[i].value.cooked;
      if (i < node.expressions.length) {
        const e = this.emitExpr(node.expressions[i]);
        if (e.pre) this.pre.push(...e.pre);
        parts.push(cur, this.toStr(e.code, e.type));
        cur = '';
      }
    }
    parts.push(cur);
    // fold into concat chain
    let code = cStr(parts[0]);
    let type = 'string';
    for (let i = 1; i < parts.length; i += 2) {
      const lhs = code;
      const rhs = cStr(parts[i + 1] === undefined ? '' : parts[i + 1]);
      code = `mochi_concat(${lhs}, ${cStr(parts[i])})`;
      // need to interleave properly: concat(lhs, part_i) then concat with next
      // We'll build sequentially:
      code = `mochi_concat(${lhs}, ${parts[i]})`;
      if (i + 1 < parts.length) code = `mochi_concat(${code}, ${cStr(parts[i + 1])})`;
    }
    return { code, type: 'string', pre: [] };
  }

  emitFunctionExpr(node) {
    const name = node.id ? node.id.name : ('fn_' + (this.fnCounter++));
    const cname = this.cName(name);
    const sym = this.analyzeFunction(node, cname);
    this.declare(name, sym);
    // generate a static function
    const params = node.params.map((p, i) => `${this.cTypeName(sym.params[i].type)} ${this.cName(p.name)}`).join(', ');
    const body = this.emitFunctionBody(node, sym);
    const fnName = cname + '_impl';
    this.fnLines = this.fnLines || [];
    this.fnLines.push(`static ${this.cTypeName(sym.type)} ${fnName}(${params}) {\n${body.join('\n')}\n}`);
    // function pointer typedef
    const tname = 'fnptr_' + (this.fnPtrCounter++);
    this.fnPtrTypedefs.push(`typedef ${this.cTypeName(sym.type)} (*${tname})(${params});`);
    return { code: `(${tname})${fnName}`, type: 'unknown', pre: [] };
  }

  emitFunctionBody(node, sym) {
    this.pushScope();
    node.params.forEach((p, i) => this.declare(p.name, { cname: this.cName(p.name), type: sym.params[i].type }));
    const body = [];
    this.ind++;
    for (const st of node.body.body) {
      const r = this.emitStmt(st);
      for (const l of r) body.push(l);
    }
    this.ind--;
    this.popScope();
    return body;
  }

  emitNew(node) {
    if (node.callee.type === 'Identifier' && (node.callee.name === 'Array' || node.callee.name === 'Object')) {
      if (node.callee.name === 'Array') {
        const arr = this.temp('arr');
        const pre = [`mochi_array *${arr} = mochi_array_new();`];
        if (node.arguments.length === 1 && node.arguments[0].type === 'Literal' && typeof node.arguments[0].value === 'number') {
          const n = node.arguments[0].value;
          for (let i = 0; i < n; i++) pre.push(`mochi_array_push(${arr}, mochi_undef());`);
        }
        return { code: arr, type: 'arr', pre };
      }
      return { code: '0', type: 'null', pre: [] };
    }
    this.warnings.push('`new` on non-Array/Object not supported');
    const callee = this.emitExpr(node.callee);
    return { code: callee.code, type: callee.type, pre: callee.pre || [] };
  }

  emitCall(node) {
    const callee = node.callee;
    // direct builtins
    if (callee.type === 'Identifier') {
      const name = callee.name;
      const args = node.arguments.map(a => this.emitExpr(a));
      const pre = args.reduce((acc, a) => acc.concat(a.pre || []), []);
      const arg = (i) => args[i] ? args[i] : { code: '0', type: 'null', pre: [] };
      switch (name) {
        case 'console': {
          // console(...) without .log — treat as error print
          const parts = args.map(a => this.toStr(a.code, a.type)).join(', ');
          this.headers.add('stdio.h');
          return { code: `(fprintf(stderr, "%s\\n", ${parts || '""'}))`, type: 'null', pre };
        }
        case 'parseInt': {
          const a = arg(0);
          this.headers.add('stdlib.h');
          const radix = arg(1);
          const rs = this.toStr(a.code, a.type);
          if (radix.type === 'int' && radix.code !== '0' && radix.code !== '10') {
            return { code: `((int)strtol(${rs}, NULL, ${radix.code}))`, type: 'int', pre };
          }
          return { code: `((int)atoi(${rs}))`, type: 'int', pre };
        }
        case 'parseFloat': {
          const a = arg(0);
          this.headers.add('stdlib.h');
          return { code: `atof(${this.toStr(a.code, a.type)})`, type: 'double', pre };
        }
        case 'Number': {
          const a = arg(0);
          if (a.type === 'string') { this.headers.add('stdlib.h'); return { code: `atof(${a.code})`, type: 'double', pre }; }
          return { code: this.toNum(a.code, a.type), type: 'double', pre };
        }
        case 'String': {
          const a = arg(0);
          return { code: this.toStr(a.code, a.type), type: 'string', pre };
        }
        case 'Boolean': {
          const a = arg(0);
          return { code: this.toBool(a.code, a.type), type: 'int', pre };
        }
        case 'isNaN': {
          const a = arg(0);
          this.needMath = true;
          return { code: `isnan(${this.toNum(a.code, a.type)})`, type: 'int', pre };
        }
        case 'isFinite': {
          const a = arg(0);
          this.needMath = true;
          return { code: `isfinite(${this.toNum(a.code, a.type)})`, type: 'int', pre };
        }
        case 'alert': {
          const a = arg(0);
          return { code: `(fprintf(stderr, "%s\\n", ${this.toStr(a.code, a.type)}))`, type: 'null', pre };
        }
        default: break;
      }
      // user function call
      const s = this.lookup(name);
      if (s && s.cname && s.params) {
        const cparams = s.params.map((p, i) => this.coerce(arg(i).code, arg(i).type, p.type)).join(', ');
        return { code: `${s.cname}(${cparams})`, type: s.type, pre };
      }
      // unknown call
      this.warnings.push('unknown call: ' + name);
      return { code: `${this.cName(name)}(${args.map(a => this.toNum(a.code, a.type)).join(', ')})`, type: 'unknown', pre };
    }

    if (callee.type === 'MemberExpression') {
      return this.emitMemberCall(node);
    }

    const c = this.emitExpr(callee);
    const args = node.arguments.map(a => this.emitExpr(a));
    const pre = args.reduce((acc, a) => acc.concat(a.pre || []), []);
    return { code: `${c.code}(${args.map(a => this.toNum(a.code, a.type)).join(', ')})`, type: 'unknown', pre: [...(c.pre || []), ...pre] };
  }

  emitMemberCall(node) {
    const m = node.callee;
    const obj = this.emitExpr(m.object);
    const pre = [...(obj.pre || [])];
    const args = node.arguments.map(a => this.emitExpr(a));
    for (const a of args) if (a.pre) pre.push(...a.pre);
    const arg = (i) => args[i] ? args[i] : { code: '0', type: 'null', pre: [] };
    const o = obj.code;

    if (m.computed) {
      const idx = this.emitExpr(m.property);
      pre.push(...(idx.pre || []));
      this.warnings.push('computed method call');
      return { code: '0', type: 'unknown', pre };
    }

    const prop = m.property.name;
    // console.log / error / warn / info
    if (obj.type === 'unknown' || (m.object.type === 'Identifier' && m.object.name === 'console')) {
      if (prop === 'log' || prop === 'info' || prop === 'error' || prop === 'warn') {
        return this.emitConsole(m, args, prop === 'error' || prop === 'warn');
      }
    }

    // Math.*
    if (m.object.type === 'Identifier' && m.object.name === 'Math') {
      return this.emitMath(prop, args, pre);
    }

    if (obj.type === 'arr') {
      switch (prop) {
        case 'push': {
          const vals = args.map(a => this.box(a.code, a.type)).join(', ');
          return { code: `mochi_array_push(${o}, ${vals})`, type: 'int', pre };
        }
        case 'pop': return { code: `mochi_array_pop(${o})`, type: 'mochi_val', pre };
        case 'join': return { code: `mochi_array_join(${o}, ${this.toStr(arg(0).code, arg(0).type)})`, type: 'string', pre };
        case 'length': return { code: `((int)((${o})->len))`, type: 'int', pre };
        case 'includes': return { code: `mochi_val_str(${o})[0] ? 1 : 0`, type: 'int', pre };
        default:
          this.warnings.push('array method: ' + prop);
          return { code: '0', type: 'unknown', pre };
      }
    }

    if (obj.type === 'string') {
      const s = o;
      switch (prop) {
        case 'toUpperCase': return { code: `mochi_toupper(${s})`, type: 'string', pre };
        case 'toLowerCase': return { code: `mochi_tolower(${s})`, type: 'string', pre };
        case 'charAt': return { code: `mochi_char_at(${s}, ${this.toInt(arg(0).code, arg(0).type)})`, type: 'string', pre };
        case 'substring': case 'substr': {
          const a = this.toInt(arg(0).code, arg(0).type);
          const b = arg(1).type === 'null' ? '-1' : this.toInt(arg(1).code, arg(1).type);
          return { code: `mochi_substr(${s}, ${a}, ${b})`, type: 'string', pre };
        }
        case 'indexOf': {
          const from = arg(1).type === 'null' ? '0' : this.toInt(arg(1).code, arg(1).type);
          return { code: `mochi_index_of(${s}, ${this.toStr(arg(0).code, arg(0).type)}, ${from})`, type: 'int', pre };
        }
        case 'trim': return { code: `mochi_trim(${s})`, type: 'string', pre };
        case 'includes': {
          this.headers.add('string.h');
          return { code: `(strstr(${s}, ${this.toStr(arg(0).code, arg(0).type)}) != NULL)`, type: 'int', pre };
        }
        case 'startsWith': {
          this.headers.add('string.h');
          return { code: `(strncmp(${s}, ${this.toStr(arg(0).code, arg(0).type)}, strlen(${this.toStr(arg(0).code, arg(0).type)})) == 0)`, type: 'int', pre };
        }
        default:
          this.warnings.push('string method: ' + prop);
          return { code: s, type: 'string', pre };
      }
    }

    if (isObj(obj.type)) {
      const fields = this.structFields[obj.type];
      const f = fields && fields[prop];
      // method call on object: treat as function pointer field if present
      if (f && f.type.startsWith('fn:')) {
        const t = f.type;
        return { code: `${o}->${prop}(${args.map(a => this.toNum(a.code, a.type)).join(', ')})`, type: 'unknown', pre };
      }
      this.warnings.push('object method call (not a data field): ' + prop);
      return { code: `${o}->${prop}`, type: f ? f.type : 'unknown', pre };
    }

    this.warnings.push('member call on ' + obj.type + ' .' + prop);
    return { code: '0', type: 'unknown', pre };
  }

  /* console.log -> puts() for a lone string, printf() otherwise. */
  emitConsole(m, args, toStderr) {
    const stream = toStderr ? 'stderr' : 'stdout';
    this.headers.add('stdio.h');
    const pre = args.reduce((acc, a) => acc.concat(a.pre || []), []);

    // Fast path: single string literal -> puts (the shortest output)
    if (args.length === 1 && args[0].type === 'string' && args[0].code.startsWith('"')) {
      const lit = args[0].code;
      if (toStderr) return { code: `(fputs(${lit}, ${stream}))`, type: 'null', pre };
      if (lit.endsWith('\\n"')) return { code: `(fputs(${lit}, ${stream}))`, type: 'null', pre };
      return { code: `(puts(${lit}))`, type: 'null', pre };
    }
    if (args.length === 0) {
      if (toStderr) return { code: `(fputs("\\n", ${stream}))`, type: 'null', pre };
      return { code: '(puts(""))', type: 'null', pre };
    }

    // Build a printf format string; escape '%' inside literal chunks only.
    let fmt = '';
    const fargs = [];
    const lit = (s) => s.replace(/%/g, '%%');
    for (const a of args) {
      if (a.type === 'string' && a.code.startsWith('"')) {
        fmt += lit(a.code.slice(1, -1));     // literal text (escape %)
      } else if (a.type === 'string') {
        fmt += '%s'; fargs.push(a.code);
      } else if (a.type === 'int' || a.type === 'bool') {
        fmt += '%d'; fargs.push(a.code);
      } else if (a.type === 'double') {
        fmt += '%g'; fargs.push(a.code);
      } else if (a.type === 'mochi_val') {
        fmt += '%s'; fargs.push(`mochi_val_str(${a.code})`);
      } else if (a.type === 'null') {
        fmt += 'null';
      } else if (a.type === 'undefined') {
        fmt += 'undefined';
      } else if (isObj(a.type)) {
        fmt += '{';
        const fields = this.structFields[a.type] || {};
        const keys = Object.keys(fields);
        keys.forEach((k, i) => {
          if (i) fmt += ', ';
          fmt += k + ': ';
          const ft = fields[k].type;
          if (ft === 'string') { fmt += '"%s"'; fargs.push(`${a.code}->${k}`); }
          else if (ft === 'int') { fmt += '%d'; fargs.push(`${a.code}->${k}`); }
          else if (ft === 'double') { fmt += '%g'; fargs.push(`${a.code}->${k}`); }
          else { fmt += '%s'; fargs.push(`mochi_val_str(${a.code}->${k})`); }
        });
        fmt += '}';
      } else if (a.type === 'arr') {
        const jt = this.temp('js');
        pre.push(`const char *${jt} = mochi_array_join(${a.code}, ", ");`);
        fmt += '%s'; fargs.push(jt);
      } else {
        fmt += '%s';
        fargs.push(this.toStr(a.code, a.type));
      }
    }
    const code = `(fprintf(${stream}, ${cStr(fmt + '\\n')}${fargs.length ? ', ' + fargs.join(', ') : ''}))`;
    return { code, type: 'null', pre };
  }

  emitMath(prop, args, pre) {
    this.needMath = true;
    const a = (i) => args[i] ? this.toNum(args[i].code, args[i].type) : '0';
    switch (prop) {
      case 'abs': return { code: `fabs(${a(0)})`, type: 'double', pre };
      case 'floor': return { code: `floor(${a(0)})`, type: 'double', pre };
      case 'ceil': return { code: `ceil(${a(0)})`, type: 'double', pre };
      case 'round': return { code: `round(${a(0)})`, type: 'double', pre };
      case 'sqrt': return { code: `sqrt(${a(0)})`, type: 'double', pre };
      case 'pow': return { code: `pow(${a(0)}, ${a(1)})`, type: 'double', pre };
      case 'min': return { code: `fmin(${a(0)}, ${a(1)})`, type: 'double', pre };
      case 'max': return { code: `fmax(${a(0)}, ${a(1)})`, type: 'double', pre };
      case 'random': return { code: `mochi_rand01()`, type: 'double', pre };
      case 'trunc': return { code: `trunc(${a(0)})`, type: 'double', pre };
      case 'sign': return { code: `(((${a(0)}) > 0) - ((${a(0)}) < 0))`, type: 'int', pre };
      default:
        this.warnings.push('Math.' + prop);
        return { code: '0', type: 'unknown', pre };
    }
  }

  /* ---------- function declaration emission ---------- */
  emitFunctionDecl(fn) {
    const name = fn.id.name;
    const sym = this.analyzeFunction(fn, this.cName(name));
    const cname = sym.cname;
    this.declare(name, sym);
    const params = sym.params.map((p, i) => `${this.cTypeName(p.type)} ${this.cName(p.name)}`).join(', ');
    const body = this.emitFunctionBody(fn, sym);
    this.ind = 0;
    const out = [`${this.cTypeName(sym.type)} ${cname}(${params}) {`];
    this.ind = 1;
    for (const l of body) out.push(l);
    out.push('}');
    this.ind = 1;
    return out;
  }
}

/* internal helper used by analyzeFunction */
function isFloatLiteral(node) {
  return node && node.type === 'Literal' && typeof node.value === 'number' && !Number.isInteger(node.value);
}

module.exports = { CGen, cStr };
