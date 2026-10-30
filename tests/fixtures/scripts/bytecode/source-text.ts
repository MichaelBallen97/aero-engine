// 4.3.2: the one observable difference of bytecode cooked with each function's source text stripped. Each function's
// toString(), name and length go into globalThis.results (BY15 cooks it, BY32 runs it from source and from bytecode).
export function plain(a: number, b: number): number {
  return a + b;
}

export class Klass {
  value = 1;
  method(x: number): number {
    return x + this.value;
  }
}

export const named = (p: number): number => p * 2;

const holder: { anon?: (q: number) => number } = {};
holder.anon = (q: number): number => q + 1;
const anonymous = holder.anon;

const describe = (f: unknown) => {
  const fn = f as { toString(): string; name: string; length: number };
  return { text: fn.toString(), name: fn.name, length: fn.length };
};

(globalThis as any).results = JSON.stringify({
  plain: describe(plain),
  klass: describe(Klass),
  method: describe(Klass.prototype.method),
  named: describe(named),
  anonymous: describe(anonymous),
});
