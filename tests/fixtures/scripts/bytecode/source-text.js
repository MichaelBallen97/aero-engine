// source-text.ts
function plain(a, b) {
  return a + b;
}
var Klass = class {
  value = 1;
  method(x) {
    return x + this.value;
  }
};
var named = (p) => p * 2;
var holder = {};
holder.anon = (q) => q + 1;
var anonymous = holder.anon;
var describe = (f) => {
  const fn = f;
  return { text: fn.toString(), name: fn.name, length: fn.length };
};
globalThis.results = JSON.stringify({
  plain: describe(plain),
  klass: describe(Klass),
  method: describe(Klass.prototype.method),
  named: describe(named),
  anonymous: describe(anonymous)
});
export {
  Klass,
  named,
  plain
};
//# sourceMappingURL=source-text.js.map
