#pragma once
#include <string>
#include <vector>

namespace haicode {

// One discovered skill markdown file. `id` is the stable key used by
// config ("skills" array) and session model_json ("skills" array).
struct SkillInfo {
    std::string id;          // filename, e.g. "git-commit.md"
    std::string name;        // frontmatter name or filename stem
    std::string description; // frontmatter description or ""
    std::string path;        // absolute path of the file
};

// Scan <B_USER_SETTINGS_DIRECTORY>/haicode/skills/ (global) and
// <project_dir>/.haicode/skills/ (project) for *.md skill files. A project
// file with the same filename shadows the global one. Result is sorted by
// name. The global dir can be overridden via $HPCODE_SKILLS_DIR (tests).
std::vector<SkillInfo> list_skills(const std::string& project_dir);

// Build the "# Skills" block appended to the system prompt: one
// "## <name> (<id>)" section per enabled skill, frontmatter stripped.
// Files are resolved project-first (same shadowing as list_skills).
// Unknown ids are skipped with a stderr warning. The total block is
// capped at 64 KB with a "[skills truncated]" marker. Returns "" when
// nothing is enabled or no enabled file can be read.
std::string build_skills_block(const std::string& project_dir,
                               const std::vector<std::string>& enabled_ids);

// Absolute path of the global skills directory
// (<settings>/haicode/skills/, or $HPCODE_SKILLS_DIR when set). Exposed for
// the UI and tests.
std::string global_skills_dir();

// Parse the leading "---" frontmatter block of a skill file. Fills
// name/description (empty when absent) and returns the body after the
// closing "---". A file with no frontmatter returns the content unchanged.
// No YAML dependency: only simple "key: value" lines for name/description.
std::string parse_skill_frontmatter(const std::string& content,
                                    std::string& name,
                                    std::string& description);

} // namespace haicode
