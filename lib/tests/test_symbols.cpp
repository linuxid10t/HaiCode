// SymbolsTool regression: function-local `static const std::regex` built by
// concatenating the `name` parameter initializes exactly once — the first
// symbols query froze the pattern, and every later query with a different
// symbol matched the wrong pattern for the rest of the process. These tests
// run several different symbol queries in one process / one ToolRegistry, so
// the stale-regex case fails before the fix and passes after.
#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>

static const std::string kWs = "/tmp/test_symbols_ws";

static void write_file(const std::string& rel, const std::string& content) {
    std::string path = kWs + "/" + rel;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[FAIL] cannot write " << path << "\n";
        std::exit(2);
    }
    f << content;
}

static haicode::ToolContext make_ctx() {
    haicode::ToolContext ctx;
    ctx.working_dir = kWs;
    return ctx;
}

static std::shared_ptr<haicode::Tool> get_symbols_tool() {
    static haicode::ToolRegistry registry;
    static bool registered = false;
    if (!registered) {
        haicode::register_builtin_tools(registry);
        registered = true;
    }
    auto tool = registry.get("symbols");
    if (!tool) {
        std::cerr << "[FAIL] symbols tool not registered\n";
        std::exit(2);
    }
    return tool;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

static std::string run_query(const std::string& name, const std::string& query,
                             const std::string& path) {
    auto tool = get_symbols_tool();
    auto result = tool->execute(
        {{"name", name}, {"query", query}, {"path", path}, {"verbose", true}},
        make_ctx());
    if (!result.success) {
        std::cerr << "[FAIL] symbols tool errored: " << result.error << "\n";
        std::exit(2);
    }
    return result.output;
}

// The regression itself: query Foo, then Bar, in one process. Pre-fix the
// second query ran against the frozen Foo pattern and found nothing.
static bool test_second_class_definition_found() {
    write_file("foo.cpp", "class Foo {\npublic:\n    int x;\n};\n");
    write_file("bar.cpp", "class Bar {\npublic:\n    int y;\n};\n");

    std::string out_foo = run_query("Foo", "definition", kWs);
    CHECK(out_foo.find("foo.cpp:1") != std::string::npos,
          "Foo definition should be found at foo.cpp:1, got:\n" + out_foo);
    CHECK(out_foo.find("class_def") != std::string::npos,
          "Foo hit should classify as class_def, got:\n" + out_foo);
    CHECK(out_foo.find("bar.cpp") == std::string::npos,
          "Foo query must not report bar.cpp hits, got:\n" + out_foo);

    std::string out_bar = run_query("Bar", "definition", kWs);
    CHECK(out_bar.find("bar.cpp:1") != std::string::npos,
          "Bar definition must still be found after querying Foo first "
          "(stale static regex regression), got:\n" + out_bar);
    CHECK(out_bar.find("class_def") != std::string::npos,
          "Bar hit should classify as class_def, got:\n" + out_bar);
    CHECK(out_bar.find("foo.cpp") == std::string::npos,
          "Bar query must not report foo.cpp hits, got:\n" + out_bar);

    std::cout << "[OK] second class definition found after a different first query\n";
    return true;
}

static bool test_second_typedef_found() {
    // The legacy typedef pattern only recognizes a name directly following
    // `typedef`/`using` (`using X = ...`, `typedef X ...`), so the common
    // `typedef int X;` shape is a pre-existing classify miss — out of scope
    // here. These forms are the ones the tool has always supported.
    write_file("types.h",
        "using FooInt = int;\n"
        "using BarPair = long;\n");

    std::string out_foo = run_query("FooInt", "definition", kWs);
    CHECK(out_foo.find("types.h:1") != std::string::npos,
          "FooInt definition should be found at types.h:1, got:\n" + out_foo);
    CHECK(out_foo.find("typedef_def") != std::string::npos,
          "FooInt hit should classify as typedef_def, got:\n" + out_foo);
    CHECK(out_foo.find("types.h:2") == std::string::npos,
          "FooInt query must not match the BarPair line, got:\n" + out_foo);

    std::string out_bar = run_query("BarPair", "definition", kWs);
    CHECK(out_bar.find("types.h:2") != std::string::npos,
          "BarPair (using) definition must still be found after querying "
          "FooInt first (stale static regex regression), got:\n" + out_bar);
    CHECK(out_bar.find("typedef_def") != std::string::npos,
          "BarPair hit should classify as typedef_def, got:\n" + out_bar);
    CHECK(out_bar.find("types.h:1") == std::string::npos,
          "BarPair query must not match the FooInt line, got:\n" + out_bar);

    std::cout << "[OK] second typedef/using found after a different first query\n";
    return true;
}

// Guards the regex-cache refactor against silently shifting classify()
// output: one file exercising every classification branch.
static bool test_classification_smoke() {
    write_file("smoke.cpp",
        "class Widget {\n"       // 1  definition (class_def)
        "public:\n"              // 2
        "    void draw();\n"     // 3  declaration (method_decl)
        "    int count;\n"       // 4  declaration (member_field)
        "};\n"                   // 5
        "\n"                     // 6
        "void helper(Widget w);\n" // 7 definition (free_func_def)
        "\n"                     // 8
        "int main() {\n"         // 9
        "    Widget w;\n"        // 10 mention
        "    w.count = 3;\n"     // 11 member_access (mem_re)
        "    w.draw();\n"        // 12 call
        "    helper(w);\n"       // 13 call
        "    return 0;\n"        // 14
        "}\n");                  // 15
    const std::string smoke = kWs + "/smoke.cpp";

    std::string out = run_query("Widget", "definition", smoke);
    CHECK(out.find("smoke.cpp:1") != std::string::npos,
          "Widget definition at line 1, got:\n" + out);
    CHECK(out.find("class_def") != std::string::npos,
          "Widget should classify as class_def, got:\n" + out);
    CHECK(out.find("1 hits in 1 files") != std::string::npos,
          "Widget definition should be exactly one hit, got:\n" + out);

    out = run_query("helper", "definition", smoke);
    CHECK(out.find("smoke.cpp:7") != std::string::npos,
          "helper definition at line 7, got:\n" + out);
    CHECK(out.find("free_func_def") != std::string::npos,
          "helper should classify as free_func_def, got:\n" + out);

    out = run_query("draw", "references", smoke);
    CHECK(out.find("smoke.cpp:3") != std::string::npos &&
          out.find("declaration") != std::string::npos,
          "draw prototype should be a declaration hit, got:\n" + out);
    CHECK(out.find("smoke.cpp:12") != std::string::npos &&
          out.find("call") != std::string::npos,
          "w.draw() should be a call hit, got:\n" + out);

    out = run_query("count", "references", smoke);
    CHECK(out.find("smoke.cpp:4") != std::string::npos,
          "count field declaration at line 4, got:\n" + out);
    CHECK(out.find("smoke.cpp:11") != std::string::npos &&
          out.find("member_access") != std::string::npos,
          "w.count should be a member_access hit, got:\n" + out);

    out = run_query("Widget", "references", smoke);
    CHECK(out.find("smoke.cpp:1") != std::string::npos &&
          out.find("definition") != std::string::npos,
          "references must keep the definition hit, got:\n" + out);
    CHECK(out.find("smoke.cpp:10") != std::string::npos &&
          out.find("mention") != std::string::npos,
          "Widget w; should be a mention hit, got:\n" + out);

    out = run_query("draw", "callers", smoke);
    CHECK(out.find("smoke.cpp:12") != std::string::npos &&
          out.find("call") != std::string::npos,
          "callers must keep the call site, got:\n" + out);
    CHECK(out.find("definition") == std::string::npos &&
          out.find("declaration") == std::string::npos,
          "callers must not report definitions/declarations, got:\n" + out);

    out = run_query("helper", "callers", smoke);
    CHECK(out.find("smoke.cpp:13") != std::string::npos,
          "helper call site at line 13, got:\n" + out);
    CHECK(out.find("free_func_def") == std::string::npos,
          "callers must not report the definition, got:\n" + out);

    out = run_query("Widget", "callers", smoke);
    CHECK(out.find("No symbols found") != std::string::npos,
          "Widget has no call/member_access sites, got:\n" + out);

    std::cout << "[OK] classification smoke: definition/declaration/call/"
                 "member_access/mention all as before\n";
    return true;
}

int main() {
    std::error_code ec;
    std::filesystem::remove_all(kWs, ec);
    std::filesystem::create_directories(kWs);

    std::cout << "=== SymbolsTool ===\n\n";
    bool ok = true;
    ok &= test_second_class_definition_found();
    ok &= test_second_typedef_found();
    ok &= test_classification_smoke();

    std::filesystem::remove_all(kWs, ec);

    std::cout << (ok ? "\nAll symbols tests passed!\n"
                     : "\nSome tests FAILED.\n");
    return ok ? 0 : 1;
}
