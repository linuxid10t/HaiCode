#include <haicode/skills.h>

#include <FindDirectory.h>
#include <Path.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>

namespace haicode {

// Total cap on the assembled skills block. A runaway skill file must not
// eat the context window (same philosophy as the 100 KB tool-output cap).
static constexpr size_t kSkillsBlockCap = 64 * 1024;

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string global_skills_dir() {
    if (const char* env = std::getenv("HPCODE_SKILLS_DIR"); env && *env)
        return env;
    BPath settings;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK) {
        BPath dir(settings);
        dir.Append("haicode");
        dir.Append("skills");
        return dir.Path();
    }
    return "";
}

std::string parse_skill_frontmatter(const std::string& content,
                                    std::string& name,
                                    std::string& description) {
    name.clear();
    description.clear();

    static const char* kFmOpen = "---";
    if (content.rfind(kFmOpen, 0) != 0) return content;
    // Need a newline right after the opening dashes.
    size_t pos = 3;
    if (pos >= content.size() || content[pos] != '\n') return content;
    ++pos;

    // Scan to the closing "---" on its own line.
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) return content; // unterminated
        std::string line = content.substr(pos, eol - pos);
        std::string t = trim(line);
        if (t == kFmOpen) {
            // Body starts after this line.
            size_t body = eol + 1;
            // Fill name/description from the collected region below.
            size_t fm_start = 4; // first char after opening "---\n"
            if (fm_start > content.size()) fm_start = content.size();
            std::istringstream fm(content.substr(fm_start, pos - fm_start));
            std::string fm_line;
            while (std::getline(fm, fm_line)) {
                size_t colon = fm_line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = trim(fm_line.substr(0, colon));
                std::string val = trim(fm_line.substr(colon + 1));
                // Strip matching surrounding quotes, either style.
                if (val.size() >= 2
                    && ((val.front() == '"' && val.back() == '"')
                     || (val.front() == '\'' && val.back() == '\'')))
                    val = val.substr(1, val.size() - 2);
                if (key == "name" && name.empty()) name = val;
                else if (key == "description" && description.empty()) {
                    // ">" or "|" starts a YAML block scalar: join the
                    // following indented lines (agentskills.io packs use
                    // folded multi-line descriptions).
                    if (val == ">" || val == "|" || val == ">-"
                            || val == "|-") {
                        std::string folded;
                        std::string cont;
                        while (std::getline(fm, cont)) {
                            std::string tc = trim(cont);
                            if (tc.empty()) continue;
                            if (!folded.empty()) folded += " ";
                            folded += tc;
                        }
                        description = folded;
                    } else {
                        description = val;
                    }
                }
            }
            return content.substr(body);
        }
        pos = eol + 1;
    }
    return content; // no closing fence: treat the whole file as body
}

// Depth-aware collection state: nearest-to-root wins on duplicate
// directory-skill names ("skills/foo/SKILL.md" beats "plugins/x/foo/SKILL.md").
struct Collected {
    SkillInfo info;
    int depth = 0;
};

static void collect_into(const std::string& dir,
                         std::map<std::string, Collected>& out,
                         int depth) {
    if (depth > 6) return;  // pack layouts are shallow; hard-stop cruft
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname == "." || fname == "..") continue;

        std::string full = dir + "/" + fname;
        struct stat st;
        if (::lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!fname.empty() && fname[0] == '.') continue;
            if (fname == "node_modules") continue;
            collect_into(full, out, depth + 1);
            continue;
        }

        if (fname.size() < 4 || fname.substr(fname.size() - 3) != ".md")
            continue;

        SkillInfo info;
        bool is_dir_skill = false;
        if (fname == "SKILL.md") {
            // Directory skill: id = parent dir name ("caveman-commit").
            std::string parent = dir;
            size_t slash = parent.rfind('/');
            std::string pname = (slash == std::string::npos)
                ? parent : parent.substr(slash + 1);
            if (pname.empty()) continue;
            info.id = pname;
            info.path = full;
            is_dir_skill = true;
        } else {
            // Loose .md files are skills only directly in the ROOT;
            // nested ones are pack documentation (README.md, CLAUDE.md).
            if (depth != 0) continue;
            info.id = fname;
            info.path = full;
        }
        std::string content = read_file(info.path);
        parse_skill_frontmatter(content, info.name, info.description);
        if (info.name.empty()) {
            // Fall back to the stem ("git-commit.md" → "git-commit",
            // "caveman/SKILL.md" → "caveman").
            std::string stem = info.id;
            if (stem.size() > 3 && stem.substr(stem.size() - 3) == ".md")
                stem = stem.substr(0, stem.size() - 3);
            info.name = stem;
        }
        auto it = out.find(info.id);
        // NB: hoist the key — in `out[info.id] = {std::move(info), depth}`
        // the RHS braced-init moves info.id away BEFORE operator[] reads it
        // (C++17 sequences E2 before E1), keying every entry under "".
        std::string key = info.id;
        if (it == out.end()) {
            out[key] = {std::move(info), depth};
        } else if (is_dir_skill) {
            // Duplicate dir-skill name: shallower path wins; equal depth
            // keeps the existing entry (readdir order is arbitrary).
            if (depth < it->second.depth)
                out[key] = {std::move(info), depth};
        } else {
            // Loose file with the same id: later call wins (project root
            // shadows global root — list_skills calls project last).
            out[key] = {std::move(info), depth};
        }
    }
    closedir(d);
}

static void collect_dir(const std::string& dir,
                        std::map<std::string, SkillInfo>& out) {
    std::map<std::string, Collected> collected;
    collect_into(dir, collected, 0);
    for (auto& [id, c] : collected) out[id] = std::move(c.info);
}

std::vector<SkillInfo> list_skills(const std::string& project_dir) {
    std::map<std::string, SkillInfo> by_id;
    std::string gdir = global_skills_dir();
    if (!gdir.empty()) collect_dir(gdir, by_id);
    if (!project_dir.empty())
        collect_dir(project_dir + "/.haicode/skills", by_id);

    std::vector<SkillInfo> result;
    result.reserve(by_id.size());
    for (auto& [id, info] : by_id) result.push_back(std::move(info));
    std::sort(result.begin(), result.end(),
              [](const SkillInfo& a, const SkillInfo& b) {
                  return a.name < b.name;
              });
    return result;
}

// Resolve one skill id to its file via discovery (handles both loose files
// and directory SKILL.md skills). Project entries shadow same-id global
// ones because collect order in list_skills runs project last.
static std::string resolve_skill_path(const std::string& project_dir,
                                      const std::string& id) {
    for (auto& sk : list_skills(project_dir)) {
        if (sk.id == id) return sk.path;
    }
    return "";
}

std::string build_skills_block(const std::string& project_dir,
                               const std::vector<std::string>& enabled_ids) {
    if (enabled_ids.empty()) return "";

    std::string block = "\n\n# Skills\n";
    size_t used = 0;
    for (auto& id : enabled_ids) {
        if (id.empty()) continue;
        std::string path = resolve_skill_path(project_dir, id);
        if (path.empty()) {
            fprintf(stderr,
                    "[skills] warning: enabled skill '%s' not found; skipping\n",
                    id.c_str());
            continue;
        }
        std::string name, desc;
        std::string body = parse_skill_frontmatter(read_file(path), name, desc);
        if (body.empty() && name.empty()) {
            fprintf(stderr,
                    "[skills] warning: enabled skill '%s' unreadable; skipping\n",
                    id.c_str());
            continue;
        }
        if (name.empty()) name = id;

        std::string section = "\n\n## " + name + " (" + id + ")\n\n" + body;
        if (used + section.size() > kSkillsBlockCap) {
            fprintf(stderr,
                    "[skills] warning: skills block exceeds %zu KB; truncating at '%s'\n",
                    kSkillsBlockCap / 1024, id.c_str());
            block += "\n\n[skills truncated]";
            return block;
        }
        block += section;
        used += section.size();
    }
    // Nothing resolvable → no block at all.
    if (used == 0) return "";
    return block;
}

bool parse_skill_invocation(const std::string& project_dir,
                            const std::string& text,
                            SkillInfo& out,
                            std::string& args) {
    args.clear();

    // First non-whitespace char must be '/'.
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || text[start] != '/') return false;

    // Token runs to the next whitespace.
    size_t end = text.find_first_of(" \t\r\n", start);
    if (end == std::string::npos) end = text.size();
    std::string cmd = text.substr(start + 1, end - start - 1);
    if (cmd.empty()) return false;

    // Tiered match: exact id first ("/caveman" → dir skill "caveman",
    // "/alpha.md" → loose file "alpha.md"), then stem ("/alpha" →
    // "alpha.md"). Exact wins so a dir skill "alpha" is not hijacked by
    // a loose "alpha.md" in the same breath.
    auto skills = list_skills(project_dir);
    const SkillInfo* hit = nullptr;
    for (auto& sk : skills)
        if (sk.id == cmd) { hit = &sk; break; }
    if (!hit) {
        for (auto& sk : skills) {
            const std::string& id = sk.id;
            if (id.size() > 3 && id.compare(id.size() - 3, 3, ".md") == 0
                    && id.compare(0, id.size() - 3, cmd) == 0) {
                hit = &sk;
                break;
            }
        }
    }
    if (!hit) return false;

    out = *hit;
    if (end < text.size())
        args = trim(text.substr(end));
    return true;
}

} // namespace haicode
