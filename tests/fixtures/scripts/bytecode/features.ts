// 4.3.2: one script per language area, results written to globalThis.results (cooked and run in BY15, BY30).
const R: Record<string, unknown> = {};
const put = (k: string, v: unknown) => { R[k] = v; };

class Counter {
  #n = 0;
  static #instances = 0;
  static label: string;
  static { Counter.label = "ctr"; }
  constructor() { Counter.#instances++; }
  inc(): number { return ++this.#n; }
  get value(): number { return this.#n; }
  set value(v: number) { this.#n = v; }
  static get instances(): number { return Counter.#instances; }
  #hidden(): string { return "h" + this.#n; }
  reveal(): string { return this.#hidden(); }
  static has(o: object): boolean { return #n in o; }
}
const c = new Counter(); c.inc(); c.inc(); c.value = c.value + 10;
put("class", [c.value, Counter.instances, Counter.label, c.reveal(), Counter.has(c), Counter.has({})]);

function makeAdders() { const fs: Array<() => number> = []; for (let i = 0; i < 3; i++) fs.push(() => i * 10); return fs.map(f => f()); }
put("closures", makeAdders());

function* gen() { const x: number = yield 1; yield x * 2; return 99; }
const g = gen(); put("generator", [g.next().value, g.next(21).value, g.next().value, g.next().done]);

async function* agen() { for (let i = 0; i < 3; i++) { await null; yield i * i; } }
async function collect() { const out: number[] = []; for await (const v of agen()) out.push(v); return out; }

const { a, b: [b1, ...bRest], ...others } = { a: 1, b: [2, 3, 4], c: 5, d: 6 };
put("destructuring", [a, b1, bRest, others, [...[1, 2], ...new Set([3, 3, 4])], Math.max(...[4, 9, 2])]);

function tag(strings: TemplateStringsArray, ...vals: unknown[]) { return strings.raw.join("|") + "#" + vals.join(","); }
put("tagged", tag`x${1}y\n${"z"}`);

const m = /(?<year>\d{4})-(?<month>\d{2})/u.exec("on 2026-10-30");
const sticky = /foo/y; sticky.lastIndex = 3;
put("regexp", [m?.groups?.year, m?.groups?.month, sticky.test("barfoo"), sticky.lastIndex,
  /(?<=\$)\d+/.exec("cost $42")?.[0], /\u{1F600}/u.test("\u{1F600}"), "a1b2c3".replace(/\d/g, d => String(Number(d) * 2)),
  [..."x-y-z".matchAll(/(\w)/g)].map(r => r[1]).join("")]);

put("bigint", [(2n ** 64n + 1n).toString(), (-(7n) / 2n).toString(), BigInt.asUintN(8, 257n).toString(), typeof 1n]);

class Res { constructor(public log: string[], public name: string) {} [Symbol.dispose]() { this.log.push("dispose " + this.name); } }
function useIt() { const log: string[] = []; { using r1 = new Res(log, "a"); using r2 = new Res(log, "b"); log.push("body"); } return log; }
put("using", useIt());

const map = new Map<unknown, number>([["k", 1], [2, 2]]); const set = new Set([1, 2, 2, 3]);
const sym = Symbol("s"); const symObj = { [sym]: 7 }; const wr = new WeakRef(symObj);
put("collections", [[...map.entries()], [...set], sym.toString(), symObj[sym], wr.deref() === symObj, Symbol.for("q") === Symbol.for("q")]);

const target = { x: 1 };
const trapped: string[] = [];
const proxy = new Proxy(target, { get(t, k, r) { trapped.push(String(k)); return Reflect.get(t, k, r); }, has(t, k) { trapped.push("has:" + String(k)); return Reflect.has(t, k); } });
put("proxy", [proxy.x, "x" in proxy, Reflect.ownKeys({ b: 1, a: 2, 1: 3 }), trapped]);

function tcf(): string[] { const out: string[] = []; try { try { throw new RangeError("r1"); } finally { out.push("inner finally"); } } catch (e) { out.push((e as Error).name + ":" + (e as Error).message); } finally { out.push("outer finally"); } return out; }
put("trycatch", tcf());

function labelled(): number[] { const out: number[] = []; outer: for (let i = 0; i < 4; i++) { for (let j = 0; j < 4; j++) { if (j === 2) continue outer; if (i === 3) break outer; out.push(i * 10 + j); } } return out; }
put("labels", labelled());

const nonBmp = "\u{1F680}😀x";
put("strings", [nonBmp.length, [...nonBmp].length, nonBmp.codePointAt(0), nonBmp.normalize("NFC") === nonBmp, "ß".toUpperCase(), "İ".toLowerCase().length]);

const d = new Date(Date.UTC(2026, 9, 30, 12, 34, 56, 789));
put("date", [d.toISOString(), d.getTime(), d.getUTCDay(), JSON.stringify(d), new Date(0).toISOString()]);

const obj = { s: "q\"u ", n: [1.5, -0, 1e21, 2 ** 53 + 1], nested: { t: true, z: null } };
put("json", [JSON.stringify(obj), JSON.stringify(JSON.parse(JSON.stringify(obj))) === JSON.stringify(obj), JSON.stringify({ b: 2, a: 1 }, null, 1)]);

put("math", [Math.sqrt(2), Math.PI * 1e10, 0.1 + 0.2, Math.fround(1.1), Math.hypot(3, 4), Math.trunc(-4.7), Math.sign(-0), Object.is(Math.round(-0.4), -0), (123.456).toFixed(2), (1e21).toString(), (255).toString(16), Number.parseFloat("1e-7").toString(), Math.imul(0xffffffff, 5), Math.clz32(1)]);

try { (null as any).boom; } catch (e) { put("caughtError", [(e as Error).name, (e as Error).message, (e as Error).stack]); }

export default class Behaviour {
  speed = 2;
  update(dt: number): number { return this.speed * dt; }
  describe(): string { return `Behaviour(${this.speed})`; }
}
const beh = new Behaviour();
put("defaultClass", [beh.update(1.5), beh.describe(), typeof Behaviour, Object.getOwnPropertyNames(Behaviour.prototype)]);

put("asyncgen", await collect());
async function delayed(): Promise<string> { await Promise.resolve(); return "awaited"; }
put("tla", await delayed());
const order: string[] = [];
Promise.resolve().then(() => order.push("micro1"));
queueMicrotask?.(() => order.push("micro2"));
await null;
put("order", order);

put("meta", [typeof import.meta, Object.keys(import.meta).length, Object.getPrototypeOf(import.meta) === null]);

(globalThis as any).results = JSON.stringify(R);
