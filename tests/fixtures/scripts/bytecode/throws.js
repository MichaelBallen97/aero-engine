// throws.ts
function inner(n) {
  if (n > 2) throw new TypeError(`too big: ${n}`);
  return inner(n + 1);
}
globalThis.results = "before";
var value = inner(0);
export {
  value
};
//# sourceMappingURL=throws.js.map
