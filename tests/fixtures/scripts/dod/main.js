// Epic 4.1's Definition of Done (task 4.1.2): an entry module that imports through the VFS, both ways.
import { add } from "./lib/math.js";
import { greeting } from "../shared/greeting.js";

globalThis.dodResult = `${greeting} ${add(2, 3)}`;
