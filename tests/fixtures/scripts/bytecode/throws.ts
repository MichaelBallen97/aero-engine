function inner(n: number): number { if (n > 2) throw new TypeError(`too big: ${n}`); return inner(n + 1); }
(globalThis as any).results = "before";
export const value = inner(0);
