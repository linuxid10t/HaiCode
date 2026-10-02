// ProposePlanTool: the plans directory is created on demand by the tool,
// never pre-created at startup (Phase 8, Task: stale tree hygiene).
#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "test_check.h"

static haicode::ToolRegistry& reg() {
    static haicode::ToolRegistry r;
    static bool done = false;
    if (!done) { haicode::register_builtin_tools(r); done = true; }
    return r;
}

static std::shared_ptr<haicode::Tool> tool(const std::string& name) {
    auto t = reg().get(name);
    TEST_REQUIRE(t, "tool not found: " + name);
    return t;
}

static bool dir_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static int count_active_plans(const std::string& plans_dir) {
    DIR* d = opendir(plans_dir.c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() < 4 || name.substr(name.size() - 3) != ".md") continue;
        std::ifstream f(plans_dir + "/" + name);
        std::string first_line;
        if (std::getline(f, first_line) &&
            first_line.find("haicode-status: active") != std::string::npos)
            ++n;
    }
    closedir(d);
    return n;
}

int main() {
    const std::string proj = "/tmp/haicode_test_propose_plan";
    (void)system(("rm -rf " + proj).c_str());
    TEST_REQUIRE(mkdir(proj.c_str(), 0755) == 0, "mkdir project");
    const std::string plans_dir = proj + "/.haicode/plans";

    haicode::ToolContext ctx;
    ctx.working_dir = proj;

    // (a) The GUI no longer pre-creates the directory at startup.
    TEST_REQUIRE(!dir_exists(plans_dir), "plans dir must not pre-exist");

    // (c) Empty plan is rejected and writes nothing.
    {
        auto res = tool("propose_plan")->execute({{"plan", ""}}, ctx);
        TEST_REQUIRE(!res.success, "empty plan rejected");
        TEST_REQUIRE(!dir_exists(plans_dir),
                     "rejected call must not create the plans dir");
    }

    // (b) A proposal creates the nested dirs and writes an active plan.
    {
        auto res = tool("propose_plan")->execute(
            {{"plan", "# Phase 9\n\nDo the thing."}}, ctx);
        TEST_REQUIRE(res.success, "plan proposal succeeds: " + res.error);
        TEST_REQUIRE(dir_exists(plans_dir), "plans dir created on demand");
        TEST_REQUIRE(count_active_plans(plans_dir) == 1, "one active plan");
        nlohmann::json out = nlohmann::json::parse(res.output);
        std::ifstream f(out.value("path", ""));
        TEST_REQUIRE(f.is_open(), "plan file readable");
        std::string first_line;
        TEST_REQUIRE(std::getline(f, first_line), "plan file not empty");
        TEST_REQUIRE(first_line == "<!-- haicode-status: active -->",
                     "status header present");
    }

    // (d) discard_plan retires the plan; no active marker remains.
    {
        auto res = tool("discard_plan")->execute({{"reason", "abandoned"}}, ctx);
        TEST_REQUIRE(res.success, "discard_plan succeeds: " + res.error);
        TEST_REQUIRE(count_active_plans(plans_dir) == 0, "no active plan left");
    }

    std::cout << "test_propose_plan: all checks passed\n";
    return 0;
}
