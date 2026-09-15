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
                else if (key == "description" && description.empty())
                    description = val;
            }
            return content.substr(body);
        }
        pos = eol + 1;
    }
    return content; // no closing fence: treat the whole file as body
}

// Collect id → path for *.md entries of one directory. Later collections
// shadow earlier ones by id, so call with project last.
static void collect_dir(const std::string& dir,
                        std::map<std::string, SkillInfo>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname.size() < 4 || fname.substr(fname.size() - 3) != ".md")
            continue;
        SkillInfo info;
        info.id = fname;
        info.path = dir + "/" + fname;
        std::string content = read_file(info.path);
        std::string body = parse_skill_frontmatter(content, info.name,
                                                   info.description);
        if (info.name.empty()) {
            // Fall back to the filename stem ("git-commit.md" →
            // "git-commit").
            info.name = fname.substr(0, fname.size() - 3);
        }
        (void)body;
        out[info.id] = std::move(info); // project entry shadows global
    }
    closedir(d);
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

// Resolve one skill id to its file: project dir shadows the global one.
static std::string resolve_skill_path(const std::string& project_dir,
                                      const std::string& id) {
    if (!project_dir.empty()) {
        std::string p = project_dir + "/.haicode/skills/" + id;
        struct stat dummy;
        if (::stat(p.c_str(), &dummy) == 0) return p;
    }
    std::string gdir = global_skills_dir();
    if (!gdir.empty()) {
        std::string p = gdir + "/" + id;
        struct stat dummy;
        if (::stat(p.c_str(), &dummy) == 0) return p;
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

} // namespace haicode
