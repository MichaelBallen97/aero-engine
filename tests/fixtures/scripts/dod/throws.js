// Fails on purpose (task 4.1.2, ML14): the error must reach the engine log once, naming this file and line.
import { add } from "./lib/math.js";

function fail(value) {
    throw new Error(`fixture failure ${value}`);
}

fail(add(1, 1));
