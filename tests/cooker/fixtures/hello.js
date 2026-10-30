// A self-contained ES module for the script cook: a template literal, an arrow, a class with a private
// field and a default export. It imports nothing, because a cooked script imports nothing.
const greet = (name) => `hello, ${name}`;

class Counter {
    #count = 0;
    next() {
        this.#count += 1;
        return this.#count;
    }
}

export const counter = new Counter();
export default greet;
