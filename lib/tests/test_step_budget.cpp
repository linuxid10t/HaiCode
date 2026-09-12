#include <haicode/engine.h>
#include <iostream>
#include <string>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static bool test_above_gate() {
    // Gate for max=50 is min(10, 25) = 10; anything above 10 is empty.
    auto out40 = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 40, 50);
    CHECK(out40.empty(), "steps_left=40 max=50 should be empty (above gate)");
    auto out11 = haicode::render_dynamic_prompt("m", "os", "/tmp", 11, 50);
    CHECK(out11.empty(), "steps_left=11 max=50 should be empty (just above gate)");
    std::cout << "[OK] above gate suppressed (steps_left=40, 11; max=50)\n";
    return true;
}

static bool test_gate_boundary() {
    // First emitted step: threshold exactly.
    auto out = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 10, 50);
    CHECK(!out.empty(), "steps_left=10 max=50 should emit the block");
    CHECK(out.find("per-session step budget") != std::string::npos,
          "boundary step should contain the base sentence");
    CHECK(out.find("Budget is getting tight (10 steps left)") != std::string::npos,
          "boundary step should contain 'Budget is getting tight (10 steps left)'");
    CHECK(out.find("CRITICAL") == std::string::npos,
          "boundary step should NOT contain CRITICAL");
    std::cout << "[OK] gate boundary (steps_left=10, max=50)\n";
    return true;
}

static bool test_half_max_rule() {
    // max=16 → threshold = min(10, 8) = 8.
    auto out9 = haicode::render_dynamic_prompt("m", "os", "/tmp", 9, 16);
    CHECK(out9.empty(), "steps_left=9 max=16 should be empty (threshold is 8)");
    auto out8 = haicode::render_dynamic_prompt("m", "os", "/tmp", 8, 16);
    CHECK(!out8.empty(), "steps_left=8 max=16 should emit the block");
    CHECK(out8.find("getting tight") != std::string::npos,
          "steps_left=8 max=16 should contain 'getting tight'");
    std::cout << "[OK] half-max rule (max=16, threshold 8)\n";
    return true;
}

static bool test_small_budget() {
    // max=6 → threshold = min(10, 3) = 3. First emission lands at 3 (CRITICAL).
    auto out4 = haicode::render_dynamic_prompt("m", "os", "/tmp", 4, 6);
    CHECK(out4.empty(), "steps_left=4 max=6 should be empty (threshold is 3)");
    auto out3 = haicode::render_dynamic_prompt("m", "os", "/tmp", 3, 6);
    CHECK(!out3.empty(), "steps_left=3 max=6 should emit the block");
    CHECK(out3.find("CRITICAL") != std::string::npos,
          "steps_left=3 max=6 should be CRITICAL (<= 4)");
    std::cout << "[OK] small budget (max=6, threshold 3, CRITICAL at first emission)\n";
    return true;
}

static bool test_floor() {
    // max=1 → threshold floored to 1; the single step must still render.
    auto out = haicode::render_dynamic_prompt("m", "os", "/tmp", 1, 1);
    CHECK(!out.empty(), "steps_left=1 max=1 should emit the block (floor)");
    CHECK(out.find("CRITICAL: only 1 step(s) left") != std::string::npos,
          "steps_left=1 max=1 should contain 'CRITICAL: only 1 step(s) left'");
    std::cout << "[OK] floor (max=1, single CRITICAL step)\n";
    return true;
}

static bool test_firm_tier() {
    auto out10 = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 10, 50);
    CHECK(out10.find("getting tight") != std::string::npos,
          "steps_left=10 should trigger firm tier");
    CHECK(out10.find("CRITICAL") == std::string::npos,
          "steps_left=10 should NOT be CRITICAL");
    auto out5 = haicode::render_dynamic_prompt("m", "os", "/tmp", 5, 50);
    CHECK(out5.find("getting tight") != std::string::npos,
          "steps_left=5 should trigger firm tier");
    CHECK(out5.find("CRITICAL") == std::string::npos,
          "steps_left=5 should NOT be CRITICAL");
    std::cout << "[OK] firm tier within window (steps_left=10, 5)\n";
    return true;
}

static bool test_critical_tier() {
    auto out4 = haicode::render_dynamic_prompt("m", "os", "/tmp", 4, 50);
    CHECK(out4.find("CRITICAL") != std::string::npos,
          "steps_left=4 should trigger critical tier");
    auto out2 = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 2, 50);
    CHECK(out2.find("CRITICAL: only 2 step(s) left") != std::string::npos,
          "critical tier should contain 'CRITICAL: only 2 step(s) left'");
    CHECK(out2.find("getting tight") == std::string::npos,
          "critical tier should NOT contain 'getting tight'");
    auto out1 = haicode::render_dynamic_prompt("m", "os", "/tmp", 1, 50);
    CHECK(out1.find("CRITICAL: only 1 step(s) left") != std::string::npos,
          "steps_left=1 should contain 'CRITICAL: only 1 step(s) left'");
    std::cout << "[OK] critical tier (steps_left=4, 2, 1; max=50)\n";
    return true;
}

static bool test_placeholder_substitution() {
    auto out = haicode::render_dynamic_prompt("claude-sonnet-4-6", "Haiku R1", "/projects/foo", 8, 50);
    CHECK(out.find("claude-sonnet-4-6") == std::string::npos,
          "{{MODEL}} should be substituted, not appear literally");
    CHECK(out.find("{{STEPS_LEFT}}") == std::string::npos,
          "{{STEPS_LEFT}} should be substituted, not appear literally");
    CHECK(out.find("8 step(s) remaining") != std::string::npos,
          "{{STEPS_LEFT}} should resolve to 8");
    std::cout << "[OK] placeholder substitution in dynamic prompt\n";
    return true;
}

int main() {
    std::cout << "=== Step Budget Gate + Escalation Tests ===\n";

    bool ok = true;
    ok &= test_above_gate();
    ok &= test_gate_boundary();
    ok &= test_half_max_rule();
    ok &= test_small_budget();
    ok &= test_floor();
    ok &= test_firm_tier();
    ok &= test_critical_tier();
    ok &= test_placeholder_substitution();

    if (ok) {
        std::cout << "\nAll step budget gate tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
