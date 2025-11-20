// SPDX-License-Identifier: MIT
//
// Use with
// #include "scripts/iterators.cocci"
// at the start of your cocci script
//
// Note that cocci has a builtin heuristic:
// ".*\\(for_?each\\|for_?all\\|iterate\\|loop\\|walk\\|scan\\|each\\|for\\)"
// Iterators matching that don't need to be declared explicitly.

@@
iterator name igt_dynamic;
iterator name igt_dynamic_f;
iterator name igt_fixture;
iterator name igt_fork;
iterator name igt_require;
iterator name igt_subtest;
iterator name igt_subtest_f;
iterator name igt_subtest_group;
iterator name igt_subtest_with_dynamic;
iterator name igt_subtest_with_dynamic_f;
iterator name igt_until_timeout;
iterator name igt_while_interruptible;
@@
struct dummy
