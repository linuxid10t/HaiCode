# Model metadata reference — context windows, output caps, prices

Reference data for **Task 26 (model metadata tables)** and the provider-compliance work in
**Phase 7** of `RELEASE_1.0_TASKS.md`. Feed this file to the agent doing that work.

- **Compiled:** 2026-10-02. Prices and model lineups change often; treat every number as
  "correct as of this date" and keep a `last_verified` comment next to the tables in code.
- **Units:** USD per **1,000,000 tokens**. Column order matches `ModelPricing`
  (`input, output, cache_read, cache_write`).
- **Window** = total context window in tokens (what `get_context_window()` should return;
  `usable_input_tokens()` already subtracts the output reserve). **Max out** = per-response
  output cap. **Vis** = accepts image input. **Rsn** = reasoning/thinking model.
  `Y`/`N` = stated by the source, `?` = the source did not say (do NOT treat `?` as `N`),
  `—` = not published or not applicable, `$0` = explicitly free.
- **Chk ✓** = the headline numbers also matched an independent web source (a lab announcement
  or pricing page snippet, or a reputable price aggregator) on 2026-10-02. No ✓ means the
  numbers come from the community-maintained LiteLLM table only
  (`BerriAI/litellm` → `model_prices_and_context_window.json`, `main` branch).
  Exception: Anthropic ✓ rows were cross-checked against Anthropic's own published model
  table, a stronger check than the secondary sources used for the other labs.

## 0. Provenance and confidence — read before trusting any number

1. The labs' own documentation sites (docs.z.ai, platform.moonshot.ai, etc.) were **not
   reachable** from the environment this was compiled in. No first-party pricing page was
   read directly. ✓ therefore means "two independent secondary sources agree", not
   "confirmed by the lab".
2. Several newer models post-date the compiler's own training data; they are included because
   multiple sources list them, but verify against the lab's pricing page before relying on them.
3. Known disagreements and suspicious fields are collected in §6. Resolve those first.
4. A reasonable acceptance step for the agent: add a unit test that every row in these tables
   resolves through `get_context_window()` / `lookup_pricing()` and returns exactly these
   numbers, so a future edit cannot silently drift.

## 1. How to apply this to the code

Current code: `lib/src/util/model_info.cpp` (`kKnownModels` window table, `kVisionModels`),
`lib/src/pricing/pricing.cpp` (`kBuiltin`, keyed `"<provider_id>:<model-prefix>"`),
`lib/include/haicode/provider.h` (`kDefaultMaxTokens = 8192`).

1. **Underestimating a window is safe; overestimating is not.** An unknown newer model that
   falls back to an older family entry gets a smaller window, so compaction triggers early —
   acceptable. The reverse would overflow the real window. Where a family's window changed
   between versions (Claude Opus 4.5 = 200K but 4.6+ = 1M; GLM 5 = 200K but 5.2+ = 1M;
   Sonnet 4.5), list **explicit per-version entries** instead of one short prefix.
   The existing `{"claude-opus-4", 200000}` entry currently gives 200K to Opus 4.6/4.7/4.8,
   which are 1M, and `{"claude-sonnet-4", 200000}` does the same to Sonnet 4.6 — fix both.
2. **Add a max-output table** (prefix → max output tokens) next to the window table and
   clamp the request's `max_tokens` to it. Where "Max out" equals "Window" in a table
   (xAI, Moonshot K2.x, Mistral, ByteDance), the source gave no separate output cap — do not
   use that value as a default `max_tokens`; use a conservative default (e.g. min(model cap,
   32K–64K for streaming)) and let the per-session Inference setting raise it.
3. **Long-context surcharges (§2) and tiers (§3) cannot be expressed by the flat
   `ModelPricing` struct.** Generalize it to an ordered list of tiers
   `{up_to_prompt_tokens, input, output, cache_read}` (flat models have one tier). The tier is
   chosen from the request's total prompt size (`input + cache_read + cache_write`). Without
   this, cost is under-reported for long sessions on OpenAI GPT-5.x/6, Gemini Pro, Grok,
   MiniMax M3, Sonnet 4.5, and all Alibaba/ByteDance models.
4. **Pricing lookup fallback chain**, most to least specific:
   `"<provider_id>:<model>"` → `"<provider type>:<model>"` → `"*:<model>"` (any provider).
   Model ids from these labs are globally unique (`glm-5.3`, `kimi-k3`, `qwen3.8-max`), and
   users reach them through arbitrary provider ids, proxies and aggregators, so ship all
   non-Anthropic/OpenAI built-ins under `*:`. Config `pricing` overrides keep winning ties.
5. **Normalize the model id before matching** (aggregators and clouds wrap the id):
   lowercase; drop a leading vendor path segment (`z-ai/glm-5.3`, `moonshotai/kimi-k3`,
   `meta-llama/...`, `nvidia/...`); drop cloud region/vendor prefixes (`us.`, `eu.`,
   `global.`, `anthropic.`, `meta.`, `amazon.`); drop `:free`, `:thinking`, `:beta` suffixes
   — and treat **`:free` as price $0** (OpenRouter free variants). Vertex dated snapshots use
   `@` (`claude-opus-4-5@20251101`).
6. **Context discovery order is unchanged** (config override → live provider discovery →
   this table). The table is only the fallback; for local/flavored servers the server's
   reported window wins. Local servers cost $0 — never apply cloud prices to a provider of
   type `ollama`/`vllm`/`lmstudio`/`llamacpp`.
7. **Vision table:** derive new `kVisionModels` prefixes from the `Vis` column. Notable: the
   DeepSeek V4 **Pro** is text-only (`N`) while **Flash** accepts images; GLM-5.3-Flash,
   Kimi K2.5+, Qwen3.8*, Muse Spark/Glimmer, Gemma 3/4, Grok 4.x, Mistral Large/Medium/Small
   and Ministral, MiMo, Doubao Seed and Nova 2 are vision-capable. Remember `?` means unknown.
8. **Cache pricing quirks.** Anthropic: 5-minute cache write = 1.25× input (the `Cache wr`
   column); **1-hour cache write = 2× input** (Opus 5.5 $8, Sonnet 5.5 $4, Haiku 4.5 $2,
   Fable 5.1 $20). OpenAI GPT-5.6+/GPT-6 list a cache-write rate (~1.25× input); earlier
   OpenAI models have none. Z.ai lists cache storage as $0. Reasoning tokens are billed at the
   output rate (already how `compute_cost` works).
9. **Time- and region-dependent prices (§6)** cannot be encoded as one number. Use the
   conservative (higher) price and record the rest in a comment.

## 2. Long-context surcharges

Above the threshold, the **whole request** is repriced at the higher rate for xAI (confirmed:
xAI bills the full request once the prompt reaches 200K). For the others the same
whole-request behavior is the common convention but was **not verified** — confirm per
provider. Threshold is measured on prompt tokens (input + cached).

| Model(s) | Threshold | Base in / out / cache rd | Above in / out / cache rd |
|---|---|---|---|
| `claude-sonnet-4-5` | > 200K | 3 / 15 / 0.3 | 6 / 22.5 / 0.6 |
| `gpt-6-astra` | > 272K | 10 / 50 / 1.0 | 20 / 75 / 2.0 |
| `gpt-6.1-sol`, `gpt-6-sol` | > 272K | 2 / 10 / 0.1 (Sol 6.1) or 0.2 | 4 / 15 / 0.2 or 0.4 |
| `gpt-6-luna` | > 272K | 0.1 / 0.5 / 0.01 | 0.2 / 0.75 / 0.02 |
| `gpt-5.6` | > 272K | 4 / 20 / 0.4 | 8 / 30 / 0.8 |
| `gpt-5.6-terra` | > 272K | 2 / 12 / 0.2 | 4 / 18 / 0.4 |
| `gpt-5.6-luna` | > 272K | 0.2 / 1.2 / 0.02 | 0.4 / 1.8 / 0.04 |
| `gpt-5.5` | > 272K | 5 / 30 / 0.5 | 10 / 45 / 1.0 |
| `gpt-5.4` | > 272K | 2.5 / 15 / 0.25 | 5 / 22.5 / 0.5 |
| `gemini-3.1-pro-preview` | > 200K | 2 / 12 / 0.2 | 4 / 18 / 0.4 |
| `gemini-2.5-pro` | > 200K | 1.25 / 10 / 0.125 | 2.5 / 15 / 0.25 |
| `grok-4.7`, `grok-4.6` | ≥ 200K | 2 / 6 / 0.5 | 4 / 12 / 1.0 |
| `grok-4.5`, `grok-build-latest` | ≥ 200K | 2 / 6 / 0.3 | 4 / 12 / 0.6 |
| `grok-4.3`, `grok-4.20-*` | ≥ 200K | 1.25 / 2.5 / 0.2 | 2.5 / 5 / 0.4 |
| `MiniMax-M3` | > 512K | 0.3 / 1.2 / 0.06 | 0.6 / 2.4 / 0.12 |

**No surcharge (flat at any length):** Claude Opus 4.6+, Sonnet 4.6+, Sonnet 5/5.5, Fable;
Gemini 3.5 Flash and other Flash models; Meta Muse Spark ("no long-context premium");
DeepSeek V4; Kimi K3; GLM; Mistral.

## 3. Context-tiered pricing (price depends on the request's input size)

Alibaba and ByteDance bill by input-size tier (tier chosen by the request's input tokens;
assumed whole-request, not verified). Values: input / output [/ cache read], USD per 1M.

| Model | 0–32K | 32K–128K | 128K–256K | 256K–1M |
|---|---|---|---|---|
| `qwen3.5-plus` | 0–256K: 0.4 / 2.4 | — | — | 0.5 / 3.0 |
| `qwen3-max` | 1.2 / 6 | 2.4 / 12 | 3 / 15 (to 252K) | — |
| `qwen3-coder-plus` | 1 / 5 / 0.1 | 1.8 / 9 / 0.18 | 3 / 15 / 0.3 | 6 / 60 / 0.6 |
| `qwen3-coder-flash` | 0.3 / 1.5 / 0.08 | 0.5 / 2.5 / 0.12 | 0.8 / 4 / 0.2 | 1.6 / 9.6 / 0.4 |
| `qwen-plus` | 0–256K: 0.4 / 1.2 | — | — | 1.2 / 3.6 |
| `qwen-flash` | 0–256K: 0.05 / 0.4 | — | — | 0.25 / 2 |
| `qwen3-vl-plus` | 0.2 / 1.6 | 0.3 / 2.4 | 0.6 / 4.8 | — |
| `doubao-seed-2-0-pro-260215`, `-code-preview` | 0.46 / 2.3 | 0.7 / 3.5 | 1.4 / 7 | — |
| `doubao-seed-2-0-lite-260215` | 0.087 / 0.52 | 0.13 / 0.78 | 0.26 / 1.6 | — |

Alibaba prices above are the Singapore (international) region; the US (Virginia) region is
cheaper (e.g. `qwen3.8-flash` $0.113 / $0.382) and mainland China differs again.

## 4. Per-model tables (generated from the source table; corrections applied are noted)

### Anthropic
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `claude-fable-5-1` | 1,000,000 | 128,000 | $10 | $50 | $0.25 | $12.5 | Y | Y | ✓ | Thinking always on; forced tool_choice 400s |
| `claude-fable-5` | 1,000,000 | 128,000 | $10 | $50 | $1 | $12.5 | Y | Y | ✓ |  |
| `claude-mythos-5-1` | 1,000,000 | 128,000 | $10 | $50 | $0.25 | $12.5 | Y | Y | ✓ | Project Glasswing access only |
| `claude-mythos-5` | 1,000,000 | 128,000 | $10 | $50 | $1 | $12.5 | Y | Y |  | Project Glasswing access only |
| `claude-opus-5-5` | 1,000,000 | 128,000 | $4 | $20 | $0.2 | $5 | Y | Y | ✓ | Default effort `medium`; thinking cannot be disabled |
| `claude-opus-5` | 1,000,000 | 128,000 | $5 | $25 | $0.5 | $6.25 | Y | Y | ✓ |  |
| `claude-opus-4-8` | 1,000,000 | 128,000 | $5 | $25 | $0.5 | $6.25 | Y | Y | ✓ |  |
| `claude-opus-4-7` | 1,000,000 | 128,000 | $5 | $25 | $0.5 | $6.25 | Y | Y | ✓ |  |
| `claude-opus-4-6` | 1,000,000 | 128,000 | $5 | $25 | $0.5 | $6.25 | Y | Y | ✓ |  |
| `claude-opus-4-5` | 200,000 | 64,000 | $5 | $25 | $0.5 | $6.25 | Y | Y |  | 200K window — NOT 1M |
| `claude-sonnet-5-5` | 1,000,000 | 128,000 | $2 | $10 | $0.2 | $2.5 | Y | Y | ✓ |  |
| `claude-sonnet-5` | 1,000,000 | 128,000 | $2 | $10 | $0.2 | $2.5 | Y | Y | ✓ |  |
| `claude-sonnet-4-6` | 1,000,000 | 128,000 | $3 | $15 | $0.3 | $3.75 | Y | Y | ✓ |  |
| `claude-sonnet-4-5` | 1,000,000 | 64,000 | $3 | $15 | $0.3 | $3.75 | Y | Y |  | DEPRECATED 2026-11-30. 1M only with long-context surcharge (see §2); 200K standard |
| `claude-haiku-4-5` | 200,000 | 64,000 | $1 | $5 | $0.1 | $1.25 | Y | Y | ✓ | Still uses `budget_tokens` thinking |

Older retired Claude ids (Opus 4.1/4, Sonnet 4, Claude 3.x) are no longer in the source
table; keep their current entries in the code (200K windows) for old sessions.

### OpenAI
Where "Window" is 1,050,000 the model has a 922,000 max-input cap (window − 128K output);
where it is 400,000 the max input is 272,000. `usable_input_tokens()` subtracts the output
reserve itself, so pass the **total window**, not the max-input figure.
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `gpt-6-astra` | 1,050,000 | 128,000 | $10 | $50 | $1 | $12.5 | Y | Y | ✓ | Max input 922,000. Surcharge >272K |
| `gpt-6.1-sol` | 1,050,000 | 128,000 | $2 | $10 | $0.1 | $2.5 | Y | Y | ✓ | Max input 922,000. Surcharge >272K |
| `gpt-6-sol` | 1,050,000 | 128,000 | $2 | $10 | $0.2 | $2.5 | Y | Y |  | Max input 922,000. Surcharge >272K |
| `gpt-6-luna` | 1,050,000 | 128,000 | $0.1 | $0.5 | $0.01 | $0.125 | Y | Y | ✓ | Max input 922,000. Surcharge >272K |
| `gpt-5.6` | 1,050,000 | 128,000 | $4 | $20 | $0.4 | $5 | Y | Y |  | Max input 922,000. Surcharge >272K |
| `gpt-5.6-terra` | 1,050,000 | 128,000 | $2 | $12 | $0.2 | $2.5 | Y | Y |  | Max input 922,000. Surcharge >272K |
| `gpt-5.6-luna` | 1,050,000 | 128,000 | $0.2 | $1.2 | $0.02 | $0.25 | Y | Y |  | Max input 922,000. Surcharge >272K |
| `gpt-5.5` | 1,050,000 | 128,000 | $5 | $30 | $0.5 | — | Y | Y |  | Surcharge >272K |
| `gpt-5.4` | 1,050,000 | 128,000 | $2.5 | $15 | $0.25 | — | Y | Y |  | Surcharge >272K |
| `gpt-5.4-mini` | 400,000 | 128,000 | $0.75 | $4.5 | $0.075 | — | Y | Y |  | Max input 272,000 |
| `gpt-5.4-nano` | 400,000 | 128,000 | $0.2 | $1.25 | $0.02 | — | Y | Y |  | Max input 272,000 |
| `gpt-5.2` | 400,000 | 128,000 | $1.75 | $14 | $0.175 | — | Y | Y |  | Max input 272,000 |
| `gpt-5` | 400,000 | 128,000 | $1.25 | $10 | $0.125 | — | Y | Y |  | Max input 272,000 |
| `gpt-5-mini` | 400,000 | 128,000 | $0.25 | $2 | $0.025 | — | Y | Y |  | Max input 272,000 |
| `gpt-5-nano` | 400,000 | 128,000 | $0.05 | $0.4 | $0.005 | — | Y | Y |  | Max input 272,000 |
| `gpt-5-pro` | 400,000 | 272,000 | $15 | $120 | — | — | Y | Y |  |  |
| `gpt-4.1` | 1,047,576 | 32,768 | $2 | $8 | $0.5 | — | Y | ? |  |  |
| `gpt-4.1-mini` | 1,047,576 | 32,768 | $0.4 | $1.6 | $0.1 | — | Y | ? |  |  |
| `gpt-4o` | 128,000 | 16,384 | $2.5 | $10 | $1.25 | — | Y | ? |  |  |
| `gpt-4o-mini` | 128,000 | 16,384 | $0.15 | $0.6 | $0.075 | — | Y | ? |  |  |
| `o3` | 200,000 | 100,000 | $2 | $8 | $0.5 | — | Y | Y |  |  |
| `o3-pro` | 200,000 | 100,000 | $20 | $80 | — | — | Y | Y |  |  |
| `o4-mini` | 200,000 | 100,000 | $1.1 | $4.4 | $0.275 | — | Y | Y |  | DEPRECATED 2026-10-23. |
| `o1` | 200,000 | 100,000 | $15 | $60 | $7.5 | — | Y | Y |  | DEPRECATED 2026-10-23. |

### Google (Gemini API / Vertex)
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `gemini-3.8-flash` | 1,048,576 | 65,536 | $0.75 | $3.75 | $0.075 | — | Y | Y | ✓ | Intro price until 2026-12-31; from 2027-01-01: $1.50 in / $7.50 out (cache rd $0.15) |
| `gemini-3.7-flash` | 1,048,576 | 65,536 | $0.75 | $3.75 | $0.075 | — | Y | Y |  |  |
| `gemini-3.6-flash` | 1,048,576 | 65,536 | $0.75 | $3.75 | $0.075 | — | Y | Y |  |  |
| `gemini-3.5-flash` | 1,048,576 | 65,536 | $1.5 | $9 | $0.15 | — | Y | Y | ✓ | Flat price at all lengths |
| `gemini-3.5-flash-lite` | 1,048,576 | 65,536 | $0.3 | $2.5 | $0.03 | — | Y | Y |  |  |
| `gemini-3.1-pro-preview` | 1,048,576 | 65,536 | $2 | $12 | $0.2 | — | Y | Y | ✓ | Surcharge >200K (see §2). One aggregator claimed 2M window; Google/LiteLLM say 1,048,576 |
| `gemini-3.1-flash-lite` | 1,048,576 | 65,536 | $0.25 | $1.5 | $0.025 | — | Y | Y |  | DEPRECATED 2027-05-07. |
| `gemini-3-flash-preview` | 1,048,576 | 65,536 | $0.5 | $3 | $0.05 | — | Y | Y |  |  |
| `gemini-2.5-pro` | 1,048,576 | 65,536 | $1.25 | $10 | $0.125 | — | Y | Y |  | Surcharge >200K (see §2) |
| `gemini-2.5-flash` | 1,048,576 | 65,536 | $0.3 | $2.5 | $0.03 | — | Y | Y |  |  |
| `gemini-2.5-flash-lite` | 1,048,576 | 65,536 | $0.1 | $0.4 | $0.01 | — | Y | Y |  |  |
| `gemma-4-31b-it` | 262,144 | 32,768 | $0 | $0 | — | — | Y | Y |  | Open weights; listed at $0 on the Gemini API |
| `gemma-3-27b-it` | 131,072 | 8,192 | $0 | $0 | — | — | Y | ? |  | Open weights; listed at $0 on the Gemini API |

### xAI
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `grok-4.7` | 500,000 | 500,000 | $2 | $6 | $0.5 | — | Y | Y | ✓ | Whole request billed at the higher rate once prompt ≥200K; US endpoint +10% |
| `grok-4.6` | 500,000 | 500,000 | $2 | $6 | $0.5 | — | Y | Y |  | Surcharge ≥200K |
| `grok-4.5` | 500,000 | 500,000 | $2 | $6 | $0.3 | — | Y | Y |  | Surcharge ≥200K |
| `grok-4.3` | 1,000,000 | 1,000,000 | $1.25 | $2.5 | $0.2 | — | Y | Y |  | Surcharge ≥200K |
| `grok-4.20-reasoning` | 1,000,000 | 1,000,000 | $1.25 | $2.5 | $0.2 | — | Y | Y |  | Surcharge ≥200K |
| `grok-4.20-non-reasoning` | 1,000,000 | 1,000,000 | $1.25 | $2.5 | $0.2 | — | Y | ? |  | Surcharge ≥200K |
| `grok-build-latest` | 500,000 | 500,000 | $2 | $6 | $0.3 | — | Y | Y |  | Coding model; surcharge ≥200K |
| `grok-code-fast-1` | 256,000 | 256,000 | $1 | $2 | $0.2 | — | Y | Y |  |  |

### Z.ai (GLM)
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `glm-5.3` | 1,000,000 | 128,000 | $1.4 | $4.4 | $0.26 | $0 | ? | Y | ✓ | LiteLLM max out 128K; Z.ai nominal 1M, practical ~131K |
| `glm-5.3-flash` | 1,048,576 | 128,000 | $0.15 | $0.5 | $0.03 | $0 | Y | Y | ✓ | Open weights (MIT). Practical output ~131K |
| `glm-5.2` | 1,000,000 | 128,000 | $1.4 | $4.4 | $0.26 | $0 | ? | Y |  |  |
| `glm-5.1` | 200,000 | 128,000 | $1.4 | $4.4 | $0.26 | $0 | ? | Y |  | Other hosts list 202,745 / 204,800 |
| `glm-5` | 200,000 | 128,000 | $1 | $3.2 | $0.2 | $0 | ? | Y |  | Aggregators show lower prices (~$0.60/$1.92); this is Z.ai direct |
| `glm-5-code` | 200,000 | 128,000 | $1.2 | $5 | $0.3 | $0 | ? | Y |  |  |
| `glm-4.7` | 200,000 | 128,000 | $0.6 | $2.2 | $0.11 | $0 | ? | Y |  |  |
| `glm-4.7-flash` | 200,000 | 128,000 | $0 | $0 | $0 | $0 | ? | Y |  | Free |
| `glm-4.6` | 200,000 | 128,000 | $0.6 | $2.2 | $0.11 | $0 | ? | Y |  |  |
| `glm-4.5` | 128,000 | 32,000 | $0.6 | $2.2 | — | — | ? | ? |  |  |
| `glm-4.5-air` | 128,000 | 32,000 | $0.2 | $1.1 | — | — | ? | ? |  |  |
| `glm-4.5-x` | 128,000 | 32,000 | $2.2 | $8.9 | — | — | ? | ? |  |  |
| `glm-4.5-airx` | 128,000 | 32,000 | $1.1 | $4.5 | — | — | ? | ? |  |  |
| `glm-4.5-flash` | 128,000 | 32,000 | $0 | $0 | — | — | ? | ? |  | Free |
| `glm-4.5v` | 128,000 | 32,000 | $0.6 | $1.8 | — | — | Y | ? |  |  |

### Moonshot AI (Kimi)
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `kimi-k3` | 1,048,576 | 131,072 | $3 | $15 | $0.3 | — | Y | Y | ✓ | Max out corrected from LiteLLM (it sets out=window) |
| `kimi-k2.7-code` | 262,144 | 262,144 | $0.95 | $4 | $0.19 | — | Y | Y |  | LiteLLM max out = window; real cap likely lower — verify |
| `kimi-k2.6` | 262,144 | 262,144 | $0.95 | $4 | $0.16 | — | Y | Y |  | LiteLLM max out = window; real cap likely lower — verify |
| `kimi-k2.5` | 262,144 | 262,144 | $0.6 | $3 | $0.1 | — | Y | Y |  | LiteLLM max out = window; real cap likely lower — verify |
| `moonshot-v1-128k` | 131,072 | 131,072 | $2 | $5 | — | — | ? | ? |  | Legacy |
| `moonshot-v1-32k` | 32,768 | 32,768 | $1 | $3 | — | — | ? | ? |  | Legacy |
| `moonshot-v1-8k` | 8,192 | 8,192 | $0.2 | $2 | — | — | ? | ? |  | Legacy |

### MiniMax
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `MiniMax-M3` | 1,000,000 | 131,072 | $0.3 | $1.2 | $0.06 | — | Y | Y | ✓ | Surcharge >512K (§2). Hard max out 524,288; 131,072 recommended |
| `MiniMax-M2.5` | 1,000,000 | 8,192 | $0.3 | $1.2 | $0.03 | $0.375 | ? | Y |  | LiteLLM max out 8,192 looks like a placeholder — verify |
| `MiniMax-M2.5-lightning` | 1,000,000 | 8,192 | $0.3 | $2.4 | $0.03 | $0.375 | ? | Y |  | Same caveat on max out |
| `MiniMax-M2.1` | 1,000,000 | 8,192 | $0.3 | $1.2 | $0.03 | $0.375 | ? | Y |  | Same caveat on max out |
| `MiniMax-M2.1-lightning` | 1,000,000 | 8,192 | $0.3 | $2.4 | $0.03 | $0.375 | ? | Y |  | Same caveat on max out |
| `MiniMax-M2` | 200,000 | 8,192 | $0.3 | $1.2 | $0.03 | $0.375 | ? | Y |  | Same caveat on max out |

### DeepSeek
`deepseek-chat` / `deepseek-reasoner` were scheduled for retirement on 2026-07-24; expect them
to be gone or aliased to V4 Flash. Since 2026-08-16 DeepSeek bills peak vs off-peak (per
secondary sources; peak windows reported as 01:00–04:00 and 06:00–10:00 UTC). The table uses
the **peak** rate so cost is never under-reported.
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `deepseek-v4-pro` | 1,000,000 | 393,216 | $1.32 | $3.96 | $0.044 | $0 | N | Y | ✓ | PEAK rate shown. Off-peak (since 2026-08-16): $0.66 / $1.98 / cache $0.022 |
| `deepseek-v4-flash` | 1,000,000 | 393,216 | $0.44 | $1.32 | $0.014 | $0 | Y | Y | ✓ | PEAK rate shown (LiteLLM $0.30/$1.20 is stale). Off-peak: $0.22 / $0.66 / cache $0.007 |
| `deepseek-chat` | 131,072 | 8,192 | $0.28 | $0.42 | $0.028 | — | ? | ? |  | DEPRECATED 2026-07-24. Legacy alias |
| `deepseek-reasoner` | 131,072 | 65,536 | $0.28 | $0.42 | $0.028 | — | ? | Y |  | DEPRECATED 2026-07-24. Legacy alias |

### Mistral
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `mistral-large-latest` | 262,144 | 262,144 | $0.5 | $1.5 | $0.05 | — | Y | ? | ✓ | = Mistral Large 3 |
| `mistral-medium-latest` | 262,144 | 262,144 | $1.5 | $7.5 | $0.15 | — | Y | Y | ✓ | = Medium 3.5 (open weights) |
| `mistral-small-latest` | 262,144 | 262,144 | $0.15 | $0.6 | $0.015 | — | Y | Y |  | = Small 4 (2603) |
| `ministral-14b-latest` | 262,144 | 262,144 | $0.2 | $0.2 | $0.02 | — | Y | ? |  |  |
| `ministral-8b-latest` | 262,144 | 262,144 | $0.15 | $0.15 | $0.015 | — | Y | ? |  |  |
| `ministral-3b-latest` | 131,072 | 131,072 | $0.1 | $0.1 | $0.01 | — | Y | ? |  |  |
| `devstral-latest` | 256,000 | 256,000 | $0.4 | $2 | $0.04 | — | ? | ? |  | Coding |
| `devstral-small-latest` | 256,000 | 256,000 | $0.1 | $0.3 | $0.01 | — | ? | ? |  | Coding |
| `codestral-latest` | 128,000 | 128,000 | $0.3 | $0.9 | $0.03 | — | ? | ? |  | Coding |
| `pixtral-large-latest` | 128,000 | 128,000 | $2 | $6 | $0.2 | — | Y | ? |  |  |
| `open-mistral-nemo` | 128,000 | 128,000 | $0.3 | $0.3 | $0.03 | — | ? | ? |  |  |

### Alibaba (Qwen) — first-party DashScope / Model Studio
Rows marked TIERED show only the first tier; see §3 for the rest. Open-weight Qwen models are
also served by many other hosts at different prices (§5).
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `qwen3.8-max` | 991,808 | 131,072 | $2 | $6 | $0.25 | — | Y | Y |  | Singapore region prices |
| `qwen3.8-flash` | 991,808 | 131,072 | $0.15 | $0.47 | $0.016 | $0.2 | Y | Y | ✓ | US (Virginia) region: $0.113 / $0.382 |
| `qwen3.8-omni-flash` | 991,808 | 131,072 | $0.15 | $0.47 | $0.016 | — | Y | Y |  |  |
| `qwen3.7-max` | 991,808 | 65,536 | $2.5 | $7.5 | $0.5 | — | ? | Y |  |  |
| `qwen3.5-plus` | 991,808 | 65,536 | $0.4 | $2.4 | — | — | Y | Y |  | TIERED (first tier shown) — see §3. |
| `qwen3-max` | 258,048 | 65,536 | $1.2 | $6 | — | — | ? | Y |  | TIERED (first tier shown) — see §3. |
| `qwen3-coder-plus` | 997,952 | 65,536 | $1 | $5 | $0.1 | — | ? | Y |  | TIERED (first tier shown) — see §3. |
| `qwen3-coder-flash` | 997,952 | 65,536 | $0.3 | $1.5 | $0.08 | — | ? | Y |  | TIERED (first tier shown) — see §3. |
| `qwen-plus` | 997,952 | 32,768 | $0.4 | $1.2 | — | — | ? | Y |  | TIERED (first tier shown) — see §3. |
| `qwen-flash` | 997,952 | 32,768 | $0.05 | $0.4 | — | — | ? | Y |  | TIERED (first tier shown) — see §3. |
| `qwen-turbo` | 1,000,000 | 16,384 | $0.05 | $0.2 | — | — | ? | Y |  |  |
| `qwen3-vl-plus` | 260,096 | 32,768 | $0.2 | $1.6 | — | — | Y | Y |  | TIERED (first tier shown) — see §3. |
| `qwen3-next-80b-a3b-instruct` | 262,144 | 65,536 | $0.15 | $1.2 | — | — | ? | ? |  | Open weights |
| `qwq-plus` | 98,304 | 8,192 | $0.8 | $2.4 | — | — | ? | Y |  |  |

### Meta (first-party API)
Meta's hosted models are the closed-weight **Muse Spark** line. Meta has said open-weight
versions are coming (Zuckerberg named Muse Spark 1.2). Open-weight models are in §5.
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `muse-spark-1.3` | 1,048,576 | 131,072 | $1.25 | $4.25 | $0.15 | — | Y | Y | ✓ | Closed weights. Cache rd from web (LiteLLM blank). Flat price, no long-context premium |
| `muse-spark-1.3-contributor` | 1,048,576 | 131,072 | $0.1 | $0.2 | $0.002 | — | Y | Y | ✓ | Cheap tier: prompts/completions used to improve Meta products |
| `muse-spark-1.2` | 1,048,576 | 131,072 | $1.25 | $4.25 | $0.15 | — | Y | Y |  | Open-weights release announced |
| `muse-spark-1.1` | 1,048,576 | 131,072 | $1.25 | $4.25 | $0.15 | — | Y | Y |  |  |

### Others (first-party or hyperscaler-only)
| Model ID | Window | Max out | In | Out | Cache rd | Cache wr | Vis | Rsn | Chk | Notes |
|---|---:|---:|---:|---:|---:|---:|:-:|:-:|:-:|---|
| `mimo-v2.6-pro` | 1,048,576 | 131,072 | $0.435 | $0.87 | $0.0036 | — | Y | Y |  | Xiaomi. |
| `mimo-v2.6-flash` | 1,048,576 | 131,072 | $0.14 | $0.28 | $0.0028 | — | Y | Y |  | Xiaomi. |
| `doubao-seed-2-1-pro-260628` | 256,000 | 256,000 | $0.8625 | $4.3125 | $0.1725 | — | Y | Y |  | ByteDance. |
| `doubao-seed-2-1-turbo-260628` | 256,000 | 256,000 | $0.4313 | $2.1562 | $0.0862 | — | Y | Y |  | ByteDance. |
| `doubao-seed-2-0-pro-260215` | 256,000 | 128,000 | $0.46 | $2.3 | — | — | Y | Y |  | ByteDance. TIERED (first tier shown) — see §3. |
| `amazon.nova-2-lite-v1:0` | 1,000,000 | 64,000 | $0.3 | $2.5 | $0.075 | — | Y | Y |  | Amazon. Bedrock only |
| `amazon.nova-2-pro-preview` | 1,000,000 | 64,000 | $2.1875 | $17.5 | $0.5469 | — | Y | Y |  | Amazon. Bedrock only |
| `command-a-03-2025` | 256,000 | 8,000 | $2.5 | $10 | — | — | ? | ? |  | Cohere. |
| `command-a-plus-05-2026` | 128,000 | 64,000 | $0 | $0 | — | — | Y | Y |  | Cohere. LiteLLM price blank/0 — treat as unknown |
| `command-r-plus-08-2024` | 128,000 | 4,096 | $2.5 | $10 | — | — | ? | ? |  | Cohere. |

## 5. Open-weight models — window and price depend on the HOST

For open-weight models there is no single price. Ship **window and vision only** as built-in
defaults; when a provider is local (`ollama`/`vllm`/`lmstudio`/`llamacpp`) cost is $0 and the
server's own reported window overrides the table. Hosted prices below are representative
ranges across DeepInfra, Together, Fireworks, Nebius, Azure AI, Bedrock, Vertex and
OpenRouter; they are for sanity checks and optional defaults only. Note how the **same model
gets a different window on different hosts** — another reason the table must be only a
fallback.

### Meta

| Model | Native window | Max out | Hosted in / out ($/1M) | Vis | Notes |
|---|---:|---:|---|:-:|---|
| `muse-glimmer-30b` | 131,072 | host-dependent (16,384 on Fireworks) | 0.30–0.35 / 1.2–1.5 | Y | Apache-2.0, 30B dense, released 2026-08-10 |
| `llama-4-maverick-17b-128e-instruct` | 1,048,576 | host-dependent | ≈0.19–0.35 / ≈0.65–1.15 on major hosts (outliers 0.05–0.63 in) | Y | Window by host: 1,048,576 (DeepInfra, Azure, Vertex, OpenRouter), 131,072 (Fireworks), 128,000 (Bedrock) |
| `llama-4-scout-17b-16e-instruct` | 10,000,000 | host-dependent | 0.09–0.25 / 0.29–0.78 | Y | Window by host: 10M (Vertex, Azure), 1,310,720 (OpenRouter), 1,048,576 (Together), 327,680 (DeepInfra), 131,072 (Fireworks), 128,000 (Bedrock) |
| `llama-3.3-70b-instruct` | 131,072 | host-dependent | ≈0.10–1.04 / ≈0.32–1.2 | N | Text only |
| `llama-3.1-405b / 70b / 8b-instruct` | 131,072 | host-dependent | varies ~100×: ≈$0.02–0.05 in for 8B up to ≈$1–5 in for 405B | N | Legacy |

Meta's own Llama API (`meta_llama`) lists windows of 128,000 (3.3-8B), 1,000,000 (Maverick)
and 10,000,000 (Scout) with a max output of 4,028 and **no published price**.

### Nvidia (Nemotron)
Nvidia's own hosted API (build.nvidia.com / NIM) is credit/free-tier based and has no
per-token price in the source table — treat Nvidia-direct as unknown/$0 and use host prices
only as a guide.

| Model | Native window | Max out | Hosted in / out ($/1M) | Vis | Notes |
|---|---:|---:|---|:-:|---|
| `nemotron-3-ultra-550b-a55b` | 1,000,000 | 65,536 | 0.50–1.00 / 2.2–3.6 | ? | Weights published 2026-06-04. Most hosts serve only 262,144 (OpenRouter, Azure AI, DeepInfra, Fireworks); Nebius 1,048,576; Together 512,288 |
| `nemotron-3-super-120b-a12b` | 262,144 | host-dependent | 0.08–0.30 / 0.40–0.90 | N | Bedrock lists 256,000 |
| `nemotron-3-nano-30b-a3b` | 262,144 | host-dependent (32K–236K) | 0.05–0.06 / 0.20–0.24 | N | Bedrock 256,000 |
| `nemotron-3.5-lightning` | 262,144 | 131,072 (OpenRouter) | 0.06–0.08 / 0.17–0.24 | N | Nebius lists 1,048,576 |
| `nemotron-3-nano-omni-30b-a3b` | 262,144 | 65,536 (OpenRouter) | 0.06 / 0.24 | Y | Omni (multimodal) variant |
| `nvidia-nemotron-nano-9b-v2` | 131,072 | 131,072 | 0.04–0.20 / 0.16–0.25 | N | |
| `nvidia-nemotron-nano-12b-v2` | 128,000 | 8,000 (Bedrock) | 0.20 / 0.60 | Y (VL variant) | |
| `llama-3.3-nemotron-super-49b-v1.5` | 131,072 | 131,072 | 0.10 / 0.40 | N | |

### Other open-weight families (details in the lab tables above)
GLM-5.3-Flash (MIT), Gemma 3/4 (Google), Mistral Medium 3.5 / Ministral (Mistral), Qwen3-Next
and Qwen3-VL (Alibaba), DeepSeek V4, Kimi K2.x. Same rule: window/vision are defaults, price
is host-dependent.

## 6. Known discrepancies and fields to verify first

1. **DeepSeek V4 pricing** — LiteLLM is stale: it shows Flash at a flat $0.30/$1.20 and Pro at
   the peak rate. Secondary sources describe a two-tier peak/off-peak scheme from 2026-08-16.
   Confirm on DeepSeek's pricing page and confirm which UTC hours are "peak".
2. **Kimi K3 max output** — LiteLLM says 1,048,576 (= window); web sources say ~131K. Table
   uses 131,072. Same suspicion for Kimi K2.5/K2.6/K2.7 (shown 262,144 = window).
3. **GLM-5.3 / 5.3-Flash max output** — nominal 1,048,576, "practical" ~131K; LiteLLM says
   128,000. Use ~128K.
4. **MiniMax M2.x max output** — LiteLLM shows 8,192 for every M2.x model, which looks like a
   placeholder. M3: hard max 524,288, 131,072 recommended.
5. **Gemini 3.1 Pro window** — one aggregator said 2M; Google/LiteLLM say 1,048,576. Table uses
   1,048,576.
6. **Gemini 3.8 Flash price** is an introductory $0.75/$3.75 that **expires 2026-12-31**
   (then $1.50/$7.50, cache read $0.15). Use the post-expiry price after that date.
7. **GLM-5 / 5.1 prices differ by source** — aggregators show ~$0.60/$1.92 for GLM-5; Z.ai
   direct (table) is $1.00/$3.20. Use direct prices for provider id `zai`; aggregators price
   independently.
8. **Regional pricing** — xAI US endpoint +10%; Alibaba US (Virginia) region is cheaper than
   Singapore; Claude/OpenAI data-residency or priority tiers cost more. Not modeled.
9. **GPT-5.4 / 5.5 "Window" 1,050,000** is the total window in LiteLLM; confirm that the
   128K output cap is inside it (as it is for GPT-6.x).
10. **`command-a-plus-05-2026`** and other entries with price $0 in the source mix "free"
    with "unknown". The source lists Z.ai `glm-4.5-flash` / `glm-4.7-flash` and the Gemma 3/4
    models on the Gemini API at $0; treat those as free, but do not treat any other $0
    (e.g. Cohere) as free without checking.
11. **Surcharge semantics** (whole request vs excess tokens) are verified only for xAI.

## 7. Not covered

Labs with no usable first-party data in the source: Baidu (ERNIE), Tencent (Hunyuan), StepFun,
Reka, IBM Granite, AI21 (Jamba), Perplexity (Sonar), Microsoft (Phi), Cohere beyond the rows
above, and Amazon Nova v1 (Premier/Pro/Lite/Micro: 1M / 300K / 300K / 128K windows). Also not
researched: each lab's request-parameter conventions for reasoning/thinking (Z.ai, DeepSeek,
Moonshot, Qwen and others each define their own on/off switch and effort scale) — Tasks 23–25
must check those against each provider's current API documentation, not this file.
