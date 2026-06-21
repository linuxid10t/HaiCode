#include <haicode/model_context_parse.h>
#include <iostream>
#include <cassert>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using haicode::ServerFlavor;
using json = nlohmann::json;

static bool test_vllm_context() {
    // vLLM /v1/models entry carries max_model_len
    json m = {
        {"id", "meta-llama/Meta-Llama-3-70B"},
        {"object", "model"},
        {"owned_by", "vllm"},
        {"max_model_len", 8192}
    };
    CHECK(haicode::parse_model_context_inline(m, ServerFlavor::VLLM) == 8192,
          "vLLM max_model_len should parse to 8192");
    std::cout << "[OK] vLLM max_model_len parsed correctly\n";
    return true;
}

static bool test_vllm_missing_field() {
    json m = {{"id", "some-model"}, {"object", "model"}};
    CHECK(haicode::parse_model_context_inline(m, ServerFlavor::VLLM) == 0,
          "vLLM without max_model_len should return 0");
    std::cout << "[OK] vLLM missing field returns 0\n";
    return true;
}

static bool test_openrouter_context() {
    // OpenRouter /v1/models entry carries context_length at top level
    json m = {
        {"id", "z-ai/glm-5.2"},
        {"context_length", 1048576},
        {"top_provider", {{"context_length", 1048576}, {"max_completion_tokens", 32768}}}
    };
    CHECK(haicode::parse_model_context_inline(m, ServerFlavor::OpenRouter) == 1048576,
          "OpenRouter context_length should parse to 1048576");
    std::cout << "[OK] OpenRouter context_length parsed correctly\n";
    return true;
}

static bool test_openrouter_null_top_provider() {
    // top_provider may be null for aggregation models; context_length is top-level
    json m = {
        {"id", "openrouter/fusion"},
        {"context_length", 1000000},
        {"top_provider", nullptr}
    };
    CHECK(haicode::parse_model_context_inline(m, ServerFlavor::OpenRouter) == 1000000,
          "OpenRouter with null top_provider should still parse top-level context_length");
    std::cout << "[OK] OpenRouter null top_provider handled\n";
    return true;
}

static bool test_lmstudio_context() {
    // LM Studio field name is unverified; try common candidates
    json m1 = {{"id", "llama-3-8b"}, {"context_length", 8192}};
    CHECK(haicode::parse_model_context_inline(m1, ServerFlavor::LMStudio) == 8192,
          "LM Studio context_length should parse");
    json m2 = {{"id", "llama-3-8b"}, {"contextLength", 8192}};
    CHECK(haicode::parse_model_context_inline(m2, ServerFlavor::LMStudio) == 8192,
          "LM Studio contextLength should parse");
    std::cout << "[OK] LM Studio context field candidates parsed\n";
    return true;
}

static bool test_lmstudio_native_models() {
    using haicode::parse_lmstudio_native_models;

    // Real /api/v1/models shape: {models:[{key,max_context_length,loaded_instances:[{config.context_length}]}]}.
    // Matched model with a loaded instance → runtime window (preferred).
    json loaded = {
        {"models", json::array({
            json{{"key", "qwen3.6-27b-mtp"}, {"max_context_length", 262144},
                 {"loaded_instances", json::array({
                     json{{"id", "qwen3.6-27b-mtp"},
                          {"config", json{{"context_length", 80000}}}}
                 })}
            },
            json{{"key", "falcon-h1r-7b"}, {"max_context_length", 262144},
                 {"loaded_instances", json::array()}}
        })}
    };
    CHECK(parse_lmstudio_native_models(loaded, "qwen3.6-27b-mtp") == 80000,
          "LM Studio matched model should return runtime window from loaded instance");

    // Matched model with empty loaded_instances → fall back to max_context_length.
    json notloaded = {
        {"models", json::array({
            json{{"key", "falcon-h1r-7b"}, {"max_context_length", 262144},
                 {"loaded_instances", json::array()}}
        })}
    };
    CHECK(parse_lmstudio_native_models(notloaded, "falcon-h1r-7b") == 262144,
          "LM Studio unloaded matched model should fall back to max_context_length");

    // No key match, but a model is loaded → return that model's runtime window.
    json nomatch_loaded = {
        {"models", json::array({
            json{{"key", "unrelated-1"}, {"max_context_length", 8192},
                 {"loaded_instances", json::array()}},
            json{{"key", "unrelated-2"}, {"max_context_length", 32768},
                 {"loaded_instances", json::array({
                     json{{"config", json{{"context_length", 16384}}}}
                 })}
            }
        })}
    };
    CHECK(parse_lmstudio_native_models(nomatch_loaded, "not-present") == 16384,
          "LM Studio with no key match should prefer first loaded model's runtime window");

    // No key match and nothing loaded → fall back to first model's max_context_length.
    json nomatch_none = {
        {"models", json::array({
            json{{"key", "unrelated-1"}, {"max_context_length", 8192},
                 {"loaded_instances", json::array()}}
        })}
    };
    CHECK(parse_lmstudio_native_models(nomatch_none, "not-present") == 8192,
          "LM Studio with no match and nothing loaded should return first model max_context_length");

    // Stringified context_length tolerated (robustness).
    json strval = {
        {"models", json::array({
            json{{"key", "m"}, {"loaded_instances", json::array({
                json{{"config", json{{"context_length", "4096"}}}}
            })}}
        })}
    };
    CHECK(parse_lmstudio_native_models(strval, "m") == 4096,
          "LM Studio should tolerate stringified context_length");

    // Empty/malformed → 0.
    CHECK(parse_lmstudio_native_models(json::array(), "anything") == 0,
          "LM Studio bare empty array should return 0");
    CHECK(parse_lmstudio_native_models(json{{"models", json::array()}}, "x") == 0,
          "LM Studio empty models list should return 0");
    CHECK(parse_lmstudio_native_models(json{{"data", json::array()}}, "x") == 0,
          "LM Studio wrong wrapper key should return 0");
    CHECK(parse_lmstudio_native_models(json("not an object"), "x") == 0,
          "LM Studio malformed input should return 0");
    CHECK(parse_lmstudio_native_models(json(42), "x") == 0,
          "LM Studio non-object/array input should return 0");

    std::cout << "[OK] LM Studio native /api/v1/models parsed correctly\n";
    return true;
}

static bool test_ollama_show() {
    // Ollama /api/show response: model_info["llama.context_length"]
    json j = {
        {"modelfile", "..."},
        {"model_info", {
            {"llama.context_length", 8192},
            {"general.architecture", "llama"}
        }}
    };
    CHECK(haicode::parse_ollama_show(j) == 8192,
          "Ollama model_info context_length should parse to 8192");
    std::cout << "[OK] Ollama /api/show context parsed correctly\n";
    return true;
}

static bool test_ollama_show_string_value() {
    // Some forks stringize the context_length value
    json j = {
        {"model_info", {{"llama.context_length", "4096"}}}
    };
    CHECK(haicode::parse_ollama_show(j) == 4096,
          "Ollama string context_length should parse to 4096");
    std::cout << "[OK] Ollama string-valued context parsed\n";
    return true;
}

static bool test_llamacpp_props() {
    // llama.cpp /props response: default_generation_settings.n_ctx
    json j = {
        {"default_generation_settings", {
            {"n_ctx", 4096},
            {"model", "llama-3-8b.gguf"}
        }}
    };
    CHECK(haicode::parse_llamacpp_props(j) == 4096,
          "llama.cpp n_ctx should parse to 4096");
    std::cout << "[OK] llama.cpp /props n_ctx parsed correctly\n";
    return true;
}

static bool test_malformed_inputs() {
    // Non-object JSON → 0 (no crash)
    CHECK(haicode::parse_model_context_inline(json::array(), ServerFlavor::VLLM) == 0,
          "array input to parse_model_context_inline should return 0");
    CHECK(haicode::parse_ollama_show(json("not an object")) == 0,
          "string input to parse_ollama_show should return 0");
    CHECK(haicode::parse_llamacpp_props(json(42)) == 0,
          "number input to parse_llamacpp_props should return 0");
    std::cout << "[OK] malformed inputs handled gracefully\n";
    return true;
}

static bool test_missing_nested_keys() {
    // Object exists but nested keys are absent → 0
    CHECK(haicode::parse_ollama_show(json{{"other", 1}}) == 0,
          "Ollama without model_info should return 0");
    CHECK(haicode::parse_ollama_show(json{{"model_info", json{{"other", 1}}}}) == 0,
          "Ollama model_info without context key should return 0");
    CHECK(haicode::parse_llamacpp_props(json{{"other", 1}}) == 0,
          "llama.cpp without default_generation_settings should return 0");
    CHECK(haicode::parse_llamacpp_props(
            json{{"default_generation_settings", json{{"other", 1}}}}) == 0,
          "llama.cpp settings without n_ctx should return 0");
    std::cout << "[OK] missing nested keys return 0\n";
    return true;
}

static bool test_generic_flavor_no_context() {
    // Generic flavor never carries context inline → always 0
    json m = {{"id", "gpt-4o"}, {"max_model_len", 128000}};
    CHECK(haicode::parse_model_context_inline(m, ServerFlavor::Generic) == 0,
          "Generic flavor should always return 0");
    std::cout << "[OK] Generic flavor returns 0 regardless of fields\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_vllm_context();
    ok &= test_vllm_missing_field();
    ok &= test_openrouter_context();
    ok &= test_openrouter_null_top_provider();
    ok &= test_lmstudio_context();
    ok &= test_lmstudio_native_models();
    ok &= test_ollama_show();
    ok &= test_ollama_show_string_value();
    ok &= test_llamacpp_props();
    ok &= test_malformed_inputs();
    ok &= test_missing_nested_keys();
    ok &= test_generic_flavor_no_context();

    if (ok) {
        std::cout << "\n=== All model_context tests passed ===\n";
        return 0;
    }
    std::cerr << "\n=== SOME TESTS FAILED ===\n";
    return 1;
}
