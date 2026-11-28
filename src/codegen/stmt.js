// stmt.js — statement emitters.
const { CGen } = require('./core');
const { cStr, promote, isFloatLiteral } = require('./helpers');

Object.assign(CGen.prototype, {
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
  },
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
  },
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
  },
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
  },
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
  },
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
  },
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
  },
  emitTry(node) {
    this.warnings.push('try/catch is best-effort: catch emits to stderr, no unwinding');
    const out = [];
    const body = this.emitBlockStmts(node.block);
    out.push(...body);
    return out;
  },
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
  },
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
  },
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
});

module.exports = { CGen };
