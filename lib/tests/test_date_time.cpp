#include <haicode/haicode.h>
#include <haicode/tool.h>
#include "test_check.h"
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <regex>

using namespace haicode;
using nlohmann::json;

static void check_output(ToolRegistry& registry, const ToolContext& ctx) {
    PermissionGate gate;
    const auto before = std::time(nullptr);
    const auto result = registry.execute("date_time", json::object(), ctx, gate);
    const auto after = std::time(nullptr);
    TEST_REQUIRE(result.success, result.error);
    TEST_REQUIRE(result.error.empty() && !result.denied, "successful result has no error");
    const auto output = json::parse(result.output);
    TEST_REQUIRE(output["unix_timestamp"].is_number_integer(), "integer Unix timestamp");
    const auto timestamp = output["unix_timestamp"].get<std::time_t>();
    TEST_REQUIRE(timestamp >= before && timestamp <= after, "timestamp is current");
    TEST_REQUIRE(output["timezone"].is_string(), "timezone is a string");

    const auto local_text = output["local"].get<std::string>();
    const auto utc_text = output["utc"].get<std::string>();
    TEST_REQUIRE(std::regex_match(local_text, std::regex(
        R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2})")),
        "local ISO timestamp includes colon-separated UTC offset");
    TEST_REQUIRE(std::regex_match(utc_text, std::regex(
        R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)")), "UTC ISO timestamp ends in Z");

    std::tm local{}, utc{};
    TEST_REQUIRE(localtime_r(&timestamp, &local), "convert local timestamp");
    TEST_REQUIRE(gmtime_r(&timestamp, &utc), "convert UTC timestamp");
    char expected_local[32], expected_utc[32], expected_zone[128]{};
    TEST_REQUIRE(std::strftime(expected_local, sizeof(expected_local),
        "%Y-%m-%dT%H:%M:%S", &local), "format expected local time");
    TEST_REQUIRE(std::strftime(expected_utc, sizeof(expected_utc),
        "%Y-%m-%dT%H:%M:%SZ", &utc), "format expected UTC time");
    std::strftime(expected_zone, sizeof(expected_zone), "%Z", &local);
    TEST_REQUIRE(local_text.substr(0, 19) == expected_local, "local fields match timestamp");
    TEST_REQUIRE(utc_text == expected_utc, "UTC fields match timestamp");
    TEST_REQUIRE(output["timezone"] == expected_zone, "timezone matches local time");

    int offset = (std::stoi(local_text.substr(20, 2)) * 60
        + std::stoi(local_text.substr(23, 2))) * 60;
    if (local_text[19] == '-') offset = -offset;
    const std::time_t shifted = timestamp + offset;
    std::tm shifted_utc{};
    TEST_REQUIRE(gmtime_r(&shifted, &shifted_utc), "convert offset-adjusted time");
    char shifted_text[32];
    TEST_REQUIRE(std::strftime(shifted_text, sizeof(shifted_text),
        "%Y-%m-%dT%H:%M:%S", &shifted_utc), "format offset-adjusted time");
    TEST_REQUIRE(local_text.substr(0, 19) == shifted_text, "offset agrees with local time");
}

int main() {
    ToolRegistry registry;
    register_builtin_tools(registry);
    const auto tool = registry.get("date_time");
    TEST_REQUIRE(tool, "date_time is registered");
    bool found = false;
    for (const auto& definition : registry.definitions()) {
        if (definition.name != "date_time") continue;
        found = true;
        TEST_REQUIRE(!definition.description.empty(), "tool has a description");
        TEST_REQUIRE(definition.input_schema["type"] == "object", "object schema");
        TEST_REQUIRE(definition.input_schema["properties"] == json::object(), "no parameters");
        TEST_REQUIRE(definition.input_schema["additionalProperties"] == false,
            "unexpected parameters are disallowed");
    }
    TEST_REQUIRE(found, "date_time is exposed to providers");

    ToolContext ctx;
    ctx.session_id = "date-time-test";
    TEST_REQUIRE(tool->resource(json::object(), ctx) == "current time", "stable resource");
    TEST_REQUIRE(!tool->execute(json::array(), ctx).success, "reject non-object input");
    TEST_REQUIRE(!tool->execute({{"unexpected", true}}, ctx).success, "reject parameters");

    const bool was_offline = offline_mode();
    for (bool offline : {false, true}) {
        set_offline_mode(offline);
        for (auto mode : {SessionMode::Build, SessionMode::Plan, SessionMode::Chat}) {
            ctx.mode = mode;
            TEST_REQUIRE(tool_available("date_time", mode), "available in every mode, offline too");
            PermissionGate gate;
            const auto decision = registry.evaluate("date_time", json::object(), ctx, gate);
            TEST_REQUIRE(decision.effect == PermissionEffect::Allow && decision.source == "builtin",
                "allowed without prompting by default");
            check_output(registry, ctx);
        }
    }
    set_offline_mode(was_offline);

    const char* old_tz = std::getenv("TZ");
    const bool had_tz = old_tz != nullptr;
    const std::string saved_tz = old_tz ? old_tz : "";
    for (const char* zone : {"UTC", "Asia/Kathmandu", "Etc/GMT+5"}) {
        TEST_REQUIRE(setenv("TZ", zone, 1) == 0, "set test timezone");
        tzset();
        check_output(registry, ctx);
        PermissionGate gate;
        const auto output = json::parse(registry.execute("date_time", json::object(), ctx, gate).output);
        const std::string expected_offset = std::string(zone) == "UTC" ? "+00:00"
            : std::string(zone) == "Asia/Kathmandu" ? "+05:45" : "-05:00";
        TEST_REQUIRE(output["local"].get<std::string>().substr(19) == expected_offset,
            "zero, fractional positive, and negative offsets format correctly");
    }
    if (had_tz) setenv("TZ", saved_tz.c_str(), 1);
    else unsetenv("TZ");
    tzset();

    for (auto mode : {SessionMode::Build, SessionMode::Plan, SessionMode::Chat}) {
        ctx.mode = mode;
        for (bool session_rule : {false, true}) {
            for (auto effect : {PermissionEffect::Allow, PermissionEffect::Deny, PermissionEffect::Ask}) {
                PermissionGate gate;
                const std::vector<PermissionRule> rules = {{"date_time", "*", effect}};
                if (session_rule) gate.set_session_rules(ctx.session_id, rules);
                else gate.set_rules(rules);
                const auto decision = registry.evaluate("date_time", json::object(), ctx, gate);
                TEST_REQUIRE(decision.effect == effect && decision.source != "builtin",
                    "explicit rules override builtin fallback");
                bool asked = false;
                gate.set_ask_callback([&](const std::string&, const std::string& action,
                        const std::string& resource, const json&) {
                    asked = true;
                    TEST_REQUIRE(action == "date_time" && resource == "current time",
                        "approval receives tool action and stable resource");
                    return PermissionEffect::Allow;
                });
                const auto result = registry.execute("date_time", json::object(), ctx, gate);
                TEST_REQUIRE(result.success == (effect != PermissionEffect::Deny),
                    "execution respects explicit rules");
                TEST_REQUIRE(result.denied == (effect == PermissionEffect::Deny), "Deny is reported");
                TEST_REQUIRE(asked == (effect == PermissionEffect::Ask), "Ask routes to approval");
            }
        }
    }
    std::cout << "[OK] date_time registration, output, availability, and permissions\n";
    return 0;
}
