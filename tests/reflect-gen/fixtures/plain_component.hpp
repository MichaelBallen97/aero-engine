#pragma once
// Task 1.1.1 fixture: self-contained AST-walk proof — NO external include, so it parses on every
// OS with only `-std=c++20` (no sysroot). Introduces the repo's first [[engine::component]] spelling
// (a WARNING under libclang today, F5 — 1.1.2 will consume it).
struct Vec3Like {
    float x;
    float y;
    float z;
};

struct [[engine::component]] Demo {
    Vec3Like position;
    float mass;
    int hitPoints;
};
