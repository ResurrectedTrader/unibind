// The first thing that has to work: bring an interpreter up, run a script,
// read the answer back.

#include "support.h"

using py_test::Eval;
using py_test::EvalInt;
using py_test::EvalText;
using py_test::Fixture;

TEST_CASE("smoke: the platform is the python backend") {
    CHECK(ub::Platform::IsInitialized());
    CHECK(ub::Platform::BackendName() == "python");
    CHECK(ub::Platform::BackendVersion().starts_with("3."));
}

TEST_CASE("smoke: an expression statement is the completion value") {
    Fixture f;
    CHECK(EvalInt(f.context, "1 + 1") == 2);
    CHECK(EvalText(f.context, "'py' + 'thon'") == "python");
}

TEST_CASE("smoke: statements before the tail run first, in the realm's globals") {
    Fixture f;
    CHECK(EvalInt(f.context, "x = 20\ndef twice(n):\n    return n * 2\ntwice(x) + 2") == 42);
    CHECK(EvalInt(f.context, "x") == 20);
}

TEST_CASE("smoke: a script with no trailing expression is undefined") {
    Fixture f;
    CHECK(Eval(f.context, "y = 1").IsUndefined());
}

TEST_CASE("smoke: a throw is caught with its message") {
    Fixture f;
    CHECK(py_test::EvalError(f.context, "raise ValueError('nope')") == "ValueError: nope");
}
