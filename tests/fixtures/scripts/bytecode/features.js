var __knownSymbol = (name, symbol) => (symbol = Symbol[name]) ? symbol : /* @__PURE__ */ Symbol.for("Symbol." + name);
var __typeError = (msg) => {
  throw TypeError(msg);
};
var __using = (stack, value, async) => {
  if (value != null) {
    if (typeof value !== "object" && typeof value !== "function") __typeError("Object expected");
    var dispose, inner;
    if (async) dispose = value[__knownSymbol("asyncDispose")];
    if (dispose === void 0) {
      dispose = value[__knownSymbol("dispose")];
      if (async) inner = dispose;
    }
    if (typeof dispose !== "function") __typeError("Object not disposable");
    if (inner) dispose = function() {
      try {
        inner.call(this);
      } catch (e) {
        return Promise.reject(e);
      }
    };
    stack.push([async, dispose, value]);
  } else if (async) {
    stack.push([async]);
  }
  return value;
};
var __callDispose = (stack, error, hasError) => {
  var E = typeof SuppressedError === "function" ? SuppressedError : function(e, s, m2, _) {
    return _ = Error(m2), _.name = "SuppressedError", _.error = e, _.suppressed = s, _;
  };
  var fail = (e) => error = hasError ? new E(e, error, "An error was suppressed during disposal") : (hasError = true, e);
  var next = (it) => {
    while (it = stack.pop()) {
      try {
        var result = it[1] && it[1].call(it[2]);
        if (it[0]) return Promise.resolve(result).then(next, (e) => (fail(e), next()));
      } catch (e) {
        fail(e);
      }
    }
    if (hasError) throw error;
  };
  return next();
};

// features.ts
var R = {};
var put = (k, v) => {
  R[k] = v;
};
var Counter = class _Counter {
  #n = 0;
  static #instances = 0;
  static label;
  static {
    _Counter.label = "ctr";
  }
  constructor() {
    _Counter.#instances++;
  }
  inc() {
    return ++this.#n;
  }
  get value() {
    return this.#n;
  }
  set value(v) {
    this.#n = v;
  }
  static get instances() {
    return _Counter.#instances;
  }
  #hidden() {
    return "h" + this.#n;
  }
  reveal() {
    return this.#hidden();
  }
  static has(o) {
    return #n in o;
  }
};
var c = new Counter();
c.inc();
c.inc();
c.value = c.value + 10;
put("class", [c.value, Counter.instances, Counter.label, c.reveal(), Counter.has(c), Counter.has({})]);
function makeAdders() {
  const fs = [];
  for (let i = 0; i < 3; i++) fs.push(() => i * 10);
  return fs.map((f) => f());
}
put("closures", makeAdders());
function* gen() {
  const x = yield 1;
  yield x * 2;
  return 99;
}
var g = gen();
put("generator", [g.next().value, g.next(21).value, g.next().value, g.next().done]);
async function* agen() {
  for (let i = 0; i < 3; i++) {
    await null;
    yield i * i;
  }
}
async function collect() {
  const out = [];
  for await (const v of agen()) out.push(v);
  return out;
}
var { a, b: [b1, ...bRest], ...others } = { a: 1, b: [2, 3, 4], c: 5, d: 6 };
put("destructuring", [a, b1, bRest, others, [...[1, 2], .../* @__PURE__ */ new Set([3, 3, 4])], Math.max(...[4, 9, 2])]);
function tag(strings, ...vals) {
  return strings.raw.join("|") + "#" + vals.join(",");
}
put("tagged", tag`x${1}y\n${"z"}`);
var m = /(?<year>\d{4})-(?<month>\d{2})/u.exec("on 2026-10-30");
var sticky = /foo/y;
sticky.lastIndex = 3;
put("regexp", [
  m?.groups?.year,
  m?.groups?.month,
  sticky.test("barfoo"),
  sticky.lastIndex,
  /(?<=\$)\d+/.exec("cost $42")?.[0],
  /\u{1F600}/u.test("\u{1F600}"),
  "a1b2c3".replace(/\d/g, (d2) => String(Number(d2) * 2)),
  [..."x-y-z".matchAll(/(\w)/g)].map((r) => r[1]).join("")
]);
put("bigint", [(2n ** 64n + 1n).toString(), (-7n / 2n).toString(), BigInt.asUintN(8, 257n).toString(), "bigint"]);
var Res = class {
  constructor(log, name) {
    this.log = log;
    this.name = name;
  }
  log;
  name;
  [Symbol.dispose]() {
    this.log.push("dispose " + this.name);
  }
};
function useIt() {
  const log = [];
  {
    var _stack = [];
    try {
      const r1 = __using(_stack, new Res(log, "a"));
      const r2 = __using(_stack, new Res(log, "b"));
      log.push("body");
    } catch (_) {
      var _error = _, _hasError = true;
    } finally {
      __callDispose(_stack, _error, _hasError);
    }
  }
  return log;
}
put("using", useIt());
var map = /* @__PURE__ */ new Map([["k", 1], [2, 2]]);
var set = /* @__PURE__ */ new Set([1, 2, 2, 3]);
var sym = /* @__PURE__ */ Symbol("s");
var symObj = { [sym]: 7 };
var wr = new WeakRef(symObj);
put("collections", [[...map.entries()], [...set], sym.toString(), symObj[sym], wr.deref() === symObj, /* @__PURE__ */ Symbol.for("q") === /* @__PURE__ */ Symbol.for("q")]);
var target = { x: 1 };
var trapped = [];
var proxy = new Proxy(target, { get(t, k, r) {
  trapped.push(String(k));
  return Reflect.get(t, k, r);
}, has(t, k) {
  trapped.push("has:" + String(k));
  return Reflect.has(t, k);
} });
put("proxy", [proxy.x, "x" in proxy, Reflect.ownKeys({ b: 1, a: 2, 1: 3 }), trapped]);
function tcf() {
  const out = [];
  try {
    try {
      throw new RangeError("r1");
    } finally {
      out.push("inner finally");
    }
  } catch (e) {
    out.push(e.name + ":" + e.message);
  } finally {
    out.push("outer finally");
  }
  return out;
}
put("trycatch", tcf());
function labelled() {
  const out = [];
  outer: for (let i = 0; i < 4; i++) {
    for (let j = 0; j < 4; j++) {
      if (j === 2) continue outer;
      if (i === 3) break outer;
      out.push(i * 10 + j);
    }
  }
  return out;
}
put("labels", labelled());
var nonBmp = "\u{1F680}\u{1F600}x";
put("strings", [nonBmp.length, [...nonBmp].length, nonBmp.codePointAt(0), nonBmp.normalize("NFC") === nonBmp, "\xDF".toUpperCase(), "\u0130".toLowerCase().length]);
var d = new Date(Date.UTC(2026, 9, 30, 12, 34, 56, 789));
put("date", [d.toISOString(), d.getTime(), d.getUTCDay(), JSON.stringify(d), (/* @__PURE__ */ new Date(0)).toISOString()]);
var obj = { s: 'q"u\u2028', n: [1.5, -0, 1e21, 2 ** 53 + 1], nested: { t: true, z: null } };
put("json", [JSON.stringify(obj), JSON.stringify(JSON.parse(JSON.stringify(obj))) === JSON.stringify(obj), JSON.stringify({ b: 2, a: 1 }, null, 1)]);
put("math", [Math.sqrt(2), Math.PI * 1e10, 0.1 + 0.2, Math.fround(1.1), Math.hypot(3, 4), Math.trunc(-4.7), Math.sign(-0), Object.is(Math.round(-0.4), -0), 123.456.toFixed(2), 1e21.toString(), 255 .toString(16), Number.parseFloat("1e-7").toString(), Math.imul(4294967295, 5), Math.clz32(1)]);
try {
  null.boom;
} catch (e) {
  put("caughtError", [e.name, e.message, e.stack]);
}
var Behaviour = class {
  speed = 2;
  update(dt) {
    return this.speed * dt;
  }
  describe() {
    return `Behaviour(${this.speed})`;
  }
};
var beh = new Behaviour();
put("defaultClass", [beh.update(1.5), beh.describe(), typeof Behaviour, Object.getOwnPropertyNames(Behaviour.prototype)]);
put("asyncgen", await collect());
async function delayed() {
  await Promise.resolve();
  return "awaited";
}
put("tla", await delayed());
var order = [];
Promise.resolve().then(() => order.push("micro1"));
queueMicrotask?.(() => order.push("micro2"));
await null;
put("order", order);
put("meta", [typeof import.meta, Object.keys(import.meta).length, Object.getPrototypeOf(import.meta) === null]);
globalThis.results = JSON.stringify(R);
export {
  Behaviour as default
};
//# sourceMappingURL=features.js.map
