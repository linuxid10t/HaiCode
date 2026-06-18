#include <haicode/engine.h>
#include <iostream>
#include <string>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static bool test_neutral_tier() {
    auto out = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 40);
    CHECK(out.find("per-session step budget") != std::string::npos,
          "neutral tier should contain the base sentence");
    CHECK(out.find("CRITICAL") == std::string::npos,
          "neutral tier should NOT contain CRITICAL");
    CHECK(out.find("getting tight") == std::string::npos,
          "neutral tier should NOT contain 'getting tight'");
    CHECK(out.find("40 step(s) remaining") != std::string::npos,
          "neutral tier should resolve {{STEPS_LEFT}} to 40");
    std::cout << "[OK] neutral tier (steps_left=40)\n";
    return true;
}

static bool test_boundary_neutral() {
    // 15 is the lowest neutral value.
    auto out = haicode::render_dynamic_prompt("m", "os", "/tmp", 15);
    CHECK(out.find("getting tight") == std::string::npos,
          "steps_left=15 should be neutral, not 'getting tight'");
    CHECK(out.find("CRITICAL") == std::string::npos,
          "steps_left=15 should not be CRITICAL");
    std::cout << "[OK] boundary neutral (steps_left=15)\n";
    return true;
}

static bool test_firm_tier() {
    auto out = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 10);
    CHECK(out.find("per-session step budget") != std::string::npos,
          "firm tier should still contain the base sentence");
    CHECK(out.find("Budget is getting tight (10 steps left)") != std::string::npos,
          "firm tier should contain 'Budget is getting tight (10 steps left)'");
    CHECK(out.find("CRITICAL") == std::string::npos,
          "firm tier should NOT contain CRITICAL");
    std::cout << "[OK] firm tier (steps_left=10)\n";
    return true;
}

static bool test_boundary_firm() {
    // 14 is the highest firm value; 5 is the lowest.
    auto out14 = haicode::render_dynamic_prompt("m", "os", "/tmp", 14);
    CHECK(out14.find("getting tight") != std::string::npos,
          "steps_left=14 should trigger firm tier");
    auto out5 = haicode::render_dynamic_prompt("m", "os", "/tmp", 5);
    CHECK(out5.find("getting tight") != std::string::npos,
          "steps_left=5 should trigger firm tier");
    CHECK(out5.find("CRITICAL") == std::string::npos,
          "steps_left=5 should NOT be CRITICAL");
    std::cout << "[OK] firm tier boundaries (steps_left=14, 5)\n";
    return true;
}

static bool test_critical_tier() {
    auto out = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 2);
    CHECK(out.find("per-session step budget") != std::string::npos,
          "critical tier should still contain the base sentence");
    CHECK(out.find("CRITICAL: only 2 step(s) left") != std::string::npos,
          "critical tier should contain 'CRITICAL: only 2 step(s) left'");
    CHECK(out.find("getting tight") == std::string::npos,
          "critical tier should NOT contain 'getting tight'");
    std::cout << "[OK] critical tier (steps_left=2)\n";
    return true;
}

static bool test_boundary_critical() {
    // 4 is the highest critical value; 1 is the lowest.
    auto out4 = haicode::render_dynamic_prompt("m", "os", "/tmp", 4);
    CHECK(out4.find("CRITICAL") != std::string::npos,
          "steps_left=4 should trigger critical tier");
    auto out1 = haicode::render_dynamic_prompt("m", "os", "/tmp", 1);
    CHECK(out1.find("CRITICAL: only 1 step(s) left") != std::string::npos,
          "steps_left=1 should contain 'CRITICAL: only 1 step(s) left'");
    std::cout << "[OK] critical tier boundaries (steps_left=4, 1)\n";
    return true;
}

static bool test_placeholder_substitution() {
    auto out = haicode::render_dynamic_prompt("claude-sonnet-4-6", "Haiku R1", "/projects/foo", 25);
    CHECK(out.find("claude-sonnet-4-6") == std::string::npos,
          "{{MODEL}} should be substituted, not appear literally");
    CHECK(out.find("25 step(s) remaining") != std::string::npos,
          "{{STEPS_LEFT}} should resolve to 25");
    std::cout << "[OK] placeholder substitution in dynamic prompt\n";
    return true;
}

int main() {
    std::cout << "=== Step Budget Escalation Tests ===\n";

    bool ok = true;
    ok &= test_neutral_tier();
    ok &= test_boundary_neutral();
    ok &= test_firm_tier();
    ok &= test_boundary_firm();
    ok &= test_critical_tier();
    ok &= test_boundary_critical();
    ok &= test_placeholder_substitution();

    if (ok) {
        std::cout << "\nAll step budget escalation tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
