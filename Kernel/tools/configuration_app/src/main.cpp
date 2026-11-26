#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <regex>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr int kInitialWidth = 1120;
constexpr int kInitialHeight = 760;
constexpr int kHeaderHeight = 74;
constexpr int kRowHeight = 72;
constexpr int kListTop = 312;
constexpr int kConfigListTop = 258;
constexpr int kConfigRowHeight = 42;

struct FileVersion
{
    std::string path;
    std::string version;
    std::string comment;
};

struct VersionConfig
{
    std::string kernel_version = "5.0.6";
    std::string base_version = "1.0.1";
    std::vector<FileVersion> files;
};

struct DriverSetting
{
    std::string key;
    std::string path;
    bool enabled = true;
};

enum class Modal
{
    Idle,
    AddFile,
    SaveConfirmation,
    RemoveConfirmation,
    EditNote
};

enum class Tab
{
    Version,
    Config
};

struct App
{
    fs::path root;
    fs::path config_path;
    fs::path general_config_path;
    VersionConfig config;
    VersionConfig saved_config;
    Display *display = nullptr;
    Window window = 0;
    GLXContext context = nullptr;
    Colormap colormap = 0;
    XFontStruct *font = nullptr;
    GLuint font_lists = 0;
    int width = kInitialWidth;
    int height = kInitialHeight;
    int scroll = 0;
    int config_scroll = 0;
    int picker_scroll = 0;
    int selected = -1;
    int picker_selected = -1;
    int edit_row = -1;
    bool editing_search = false;
    bool editing_base = false;
    bool dirty = false;
    bool exit_after_save = false;
    bool picker_path_focused = false;
    std::string search;
    std::string picker_search;
    std::string picker_path;
    std::string edit_value;
    std::string note_value;
    std::string status;
    std::string config_error;
    std::string general_config_error;
    std::vector<DriverSetting> driver_settings;
    bool version_config_available = false;
    Modal modal = Modal::Idle;
    Tab tab = Tab::Version;
    std::vector<std::string> candidates;
};

struct Color
{
    float r, g, b, a;
};

bool file_exists_in_root(const App &app, const std::string &relative);

const Color kBackground{0.965f, 0.969f, 0.980f, 1.0f};
const Color kPanel{1.0f, 1.0f, 1.0f, 1.0f};
const Color kPanelLight{0.925f, 0.933f, 0.949f, 1.0f};
const Color kAccent{0.32f, 0.25f, 0.91f, 1.0f};
const Color kText{0.13f, 0.15f, 0.19f, 1.0f};
const Color kMuted{0.48f, 0.51f, 0.57f, 1.0f};
const Color kGreen{0.16f, 0.58f, 0.37f, 1.0f};
const Color kRed{0.77f, 0.23f, 0.26f, 1.0f};
const Color kShadow{0.04f, 0.06f, 0.10f, 0.08f};
const Color kAccentWash{0.94f, 0.93f, 1.0f, 1.0f};

std::string trim(const std::string &text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return text;
}

bool valid_version(const std::string &version)
{
    static const std::regex pattern(R"(^[0-9]+(\.[0-9]+){2}$)");
    return std::regex_match(version, pattern);
}

bool is_excluded_path(const fs::path &path)
{
    const fs::path normalized = path.lexically_normal();
    for (const auto &part : normalized)
    {
        if (part == "build")
            return true;
    }
    return normalized == "Version.cfg" || normalized == "log.txt";
}

bool is_safe_relative_path(const std::string &value, fs::path &result)
{
    if (value.empty())
        return false;
    fs::path path(value);
    if (path.is_absolute())
        return false;
    path = path.lexically_normal();
    for (const auto &part : path)
    {
        if (part == "..")
            return false;
    }
    if (path.empty() || path == "." || is_excluded_path(path))
        return false;
    result = path;
    return true;
}

std::optional<fs::path> find_root(fs::path start)
{
    std::error_code error;
    start = fs::absolute(start, error);
    if (error)
        return std::nullopt;
    if (!fs::is_directory(start, error))
        start = start.parent_path();

    while (!start.empty())
    {
        if (fs::is_regular_file(start / "Kernel" / "Version.cfg", error) ||
            fs::is_directory(start / "Kernel" / "tools" /
                                 "configuration_app",
                             error))
            return start;
        const fs::path parent = start.parent_path();
        if (parent == start)
            break;
        start = parent;
    }
    return std::nullopt;
}

bool load_config(const fs::path &path, VersionConfig &config, std::string &error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "Cannot open " + path.string();
        return false;
    }

    VersionConfig parsed;
    bool in_files = false;
    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        std::string value = trim(line);
        if (value.empty() || value[0] == '#')
            continue;
        std::string comment;
        const auto comment_start = value.find('#');
        if (comment_start != std::string::npos)
        {
            comment = trim(value.substr(comment_start + 1));
            value = trim(value.substr(0, comment_start));
        }
        if (value == "[files]")
        {
            in_files = true;
            continue;
        }

        const auto separator = value.find('=');
        if (separator == std::string::npos)
        {
            error = "Invalid config syntax at line " +
                    std::to_string(line_number);
            return false;
        }
        const std::string key = trim(value.substr(0, separator));
        const std::string item = trim(value.substr(separator + 1));
        if (key.empty() || item.empty())
        {
            error = "Empty key or value at line " +
                    std::to_string(line_number);
            return false;
        }

        if (!in_files && key == "kernel_version")
            parsed.kernel_version = item;
        else if (!in_files && key == "base_version")
            parsed.base_version = item;
        else if (in_files)
        {
            fs::path relative;
            if (!is_safe_relative_path(key, relative))
            {
                error = "Unsafe or excluded path at line " +
                        std::to_string(line_number);
                return false;
            }
            if (!valid_version(item))
            {
                error = "Invalid version at line " +
                        std::to_string(line_number) + ": " + item;
                return false;
            }
            const std::string normalized = relative.generic_string();
            const auto existing = std::find_if(
                parsed.files.begin(), parsed.files.end(),
                [&](const FileVersion &file) { return file.path == normalized; });
            if (existing != parsed.files.end())
            {
                error = "Duplicate file entry: " + normalized;
                return false;
            }
            parsed.files.push_back({normalized, item, comment});
        }
        else
        {
            error = "Unknown config setting at line " +
                    std::to_string(line_number) + ": " + key;
            return false;
        }
    }

    if (!valid_version(parsed.kernel_version))
    {
        error = "kernel_version must use x.y.z format";
        return false;
    }
    if (!valid_version(parsed.base_version))
    {
        error = "base_version must use x.y.z format";
        return false;
    }
    config = std::move(parsed);
    return true;
}

bool load_build_config(const fs::path &path, std::vector<DriverSetting> &drivers,
                       std::string &error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "Cannot open " + path.string();
        return false;
    }

    constexpr char prefix[] = "DRIVER_ENABLE_";
    std::vector<DriverSetting> parsed;
    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        const std::string value = trim(line);
        if (value.empty() || value[0] == '#')
            continue;
        const auto separator = value.find('=');
        if (separator == std::string::npos)
            continue;

        const std::string key = trim(value.substr(0, separator));
        if (key.compare(0, sizeof(prefix) - 1, prefix) != 0)
            continue;

        const std::string driver_path =
            key.substr(sizeof(prefix) - 1);
        fs::path safe_path;
        const std::string enabled = lower(trim(value.substr(separator + 1)));
        if (!is_safe_relative_path(driver_path, safe_path) ||
            safe_path.extension() != ".c" ||
            (enabled != "y" && enabled != "n"))
        {
            error = "Invalid driver setting at line " +
                    std::to_string(line_number);
            return false;
        }
        if (std::any_of(parsed.begin(), parsed.end(),
                        [&](const DriverSetting &setting) {
                            return setting.key == key;
                        }))
        {
            error = "Duplicate driver setting at line " +
                    std::to_string(line_number);
            return false;
        }
        parsed.push_back({key, safe_path.generic_string(), enabled == "y"});
    }
    if (parsed.empty())
    {
        error = "No DRIVER_ENABLE_ settings found in " + path.string();
        return false;
    }
    drivers = std::move(parsed);
    return true;
}

bool save_driver_setting(const fs::path &path, const DriverSetting &setting,
                        std::string &error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "Cannot open " + path.string();
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(input, line))
    {
        const auto separator = line.find('=');
        if (separator != std::string::npos &&
            trim(line.substr(0, separator)) == setting.key)
        {
            line = setting.key + " = " + (setting.enabled ? "y" : "n");
            found = true;
        }
        lines.push_back(std::move(line));
    }
    if (!found)
    {
        error = "Driver setting no longer exists: " + setting.key;
        return false;
    }

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
        {
            error = "Cannot create temporary config file: " + temporary.string();
            return false;
        }
        for (const std::string &saved_line : lines)
            output << saved_line << '\n';
        output.flush();
        if (!output)
        {
            error = "Cannot write temporary config file: " + temporary.string();
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }

    std::error_code rename_error;
    fs::rename(temporary, path, rename_error);
    if (rename_error)
    {
        error = "Cannot replace " + path.string() + ": " +
                rename_error.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool save_config(const App &app, std::string &error)
{
    const fs::path temporary = app.config_path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
        {
            error = "Cannot create temporary config file";
            return false;
        }
        output << "kernel_version = " << app.config.kernel_version << '\n'
               << "base_version = " << app.config.base_version << "\n\n"
               << "[files]\n";
        for (const FileVersion &file : app.config.files)
        {
            fs::path relative;
            if (!is_safe_relative_path(file.path, relative) ||
                !valid_version(file.version) ||
                (file.version == "0.0.0" && file.comment.empty()))
            {
                error = file.version == "0.0.0" && file.comment.empty()
                            ? "Version 0.0.0 needs a comment: " + file.path
                            : "Invalid file entry: " + file.path;
                output.close();
                std::error_code ignored;
                fs::remove(temporary, ignored);
                return false;
            }
            if (!file_exists_in_root(app, relative.generic_string()))
            {
                error = "Remove the missing file entry before saving: " + file.path;
                output.close();
                std::error_code ignored;
                fs::remove(temporary, ignored);
                return false;
            }
            output << relative.generic_string() << " = " << file.version;
            if (!file.comment.empty())
                output << " # " << file.comment;
            output << '\n';
        }
        output.flush();
        if (!output)
        {
            error = "Failed while writing config";
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(temporary, app.config_path, ec);
    if (ec)
    {
        error = "Cannot replace Version.cfg: " + ec.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::vector<std::string> discover_files(const App &app)
{
    std::vector<std::string> files;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(
        app.root / "Kernel", fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        const fs::path relative = fs::relative(iterator->path(), app.root, ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        const fs::path kernel_relative = relative.lexically_relative("Kernel");
        if (iterator->is_directory(ec) && kernel_relative.begin() != kernel_relative.end() &&
            *kernel_relative.begin() == "build")
        {
            iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_regular_file(ec) && !is_excluded_path(kernel_relative))
            files.push_back(kernel_relative.generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool file_exists_in_root(const App &app, const std::string &relative)
{
    fs::path safe;
    if (!is_safe_relative_path(relative, safe))
        return false;
    std::error_code ec;
    const fs::path kernel_root = fs::canonical(app.root / "Kernel", ec);
    if (ec)
        return false;
    const fs::path resolved = fs::canonical(kernel_root / safe, ec);
    if (ec || !fs::is_regular_file(resolved, ec) || ec)
        return false;
    auto root_part = kernel_root.begin();
    auto file_part = resolved.begin();
    for (; root_part != kernel_root.end() && file_part != resolved.end();
         ++root_part, ++file_part)
    {
        if (*root_part != *file_part)
            return false;
    }
    return root_part == kernel_root.end();
}

bool dirty_config(const App &app)
{
    if (app.config.kernel_version != app.saved_config.kernel_version ||
        app.config.base_version != app.saved_config.base_version ||
        app.config.files.size() != app.saved_config.files.size())
        return true;
    for (size_t i = 0; i < app.config.files.size(); ++i)
    {
        if (app.config.files[i].path != app.saved_config.files[i].path ||
            app.config.files[i].version != app.saved_config.files[i].version ||
            app.config.files[i].comment != app.saved_config.files[i].comment)
            return true;
    }
    return false;
}

void color(Color value)
{
    glColor4f(value.r, value.g, value.b, value.a);
}

void rect(float x, float y, float width, float height, Color fill)
{
    color(fill);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + width, y);
    glVertex2f(x + width, y + height);
    glVertex2f(x, y + height);
    glEnd();
}

void outline(float x, float y, float width, float height, Color edge)
{
    color(edge);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + width, y);
    glVertex2f(x + width, y + height);
    glVertex2f(x, y + height);
    glEnd();
}

void circle(float cx, float cy, float radius, Color fill);

void rounded_rect(float x, float y, float width, float height, float radius,
                  Color fill, bool border = false)
{
    constexpr int segments = 8;
    radius = std::max(0.0f, std::min(radius, std::min(width, height) / 2.0f));
    const std::array<std::pair<float, float>, 4> centers = {{
        {x + width - radius, y + radius},
        {x + width - radius, y + height - radius},
        {x + radius, y + height - radius},
        {x + radius, y + radius},
    }};
    const std::array<float, 4> starts = {{-90.0f, 0.0f, 90.0f, 180.0f}};
    if (!border)
    {
        rect(x + radius, y, width - 2.0f * radius, height, fill);
        rect(x, y + radius, width, height - 2.0f * radius, fill);
        for (const auto &center : centers)
            circle(center.first, center.second, radius, fill);
        return;
    }

    color(fill);
    glBegin(GL_LINE_LOOP);
    for (size_t corner = 0; corner < centers.size(); ++corner)
    {
        for (int step = 0; step <= segments; ++step)
        {
            const float angle =
                (starts[corner] + 90.0f * step / segments) * 0.01745329252f;
            glVertex2f(centers[corner].first + std::cos(angle) * radius,
                       centers[corner].second + std::sin(angle) * radius);
        }
    }
    glEnd();
}

void circle(float cx, float cy, float radius, Color fill)
{
    constexpr int segments = 24;
    color(fill);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int step = 0; step <= segments; ++step)
    {
        const float angle = 6.28318530718f * step / segments;
        glVertex2f(cx + std::cos(angle) * radius,
                   cy + std::sin(angle) * radius);
    }
    glEnd();
}

void text(const App &app, float x, float y, const std::string &value,
          Color tint = kText)
{
    if (app.font_lists == 0 || value.empty())
        return;
    glColor4f(tint.r, tint.g, tint.b, tint.a);
    glRasterPos2f(x, y);
    glListBase(app.font_lists);
    glCallLists(static_cast<GLsizei>(value.size()), GL_UNSIGNED_BYTE,
                value.data());
}

void rounded_panel(float x, float y, float width, float height, Color fill,
                   Color border)
{
    rounded_rect(x, y + 3, width, height, 12.0f, kShadow);
    rounded_rect(x, y, width, height, 12.0f, fill);
    rounded_rect(x, y, width, height, 12.0f, border, true);
}

std::vector<size_t> visible_rows(const App &app)
{
    std::vector<size_t> rows;
    const std::string query = lower(app.search);
    for (size_t i = 0; i < app.config.files.size(); ++i)
    {
        const FileVersion &file = app.config.files[i];
        if (query.empty() || lower(file.path).find(query) != std::string::npos ||
            lower(file.version).find(query) != std::string::npos)
            rows.push_back(i);
    }
    return rows;
}

std::vector<std::string> filtered_candidates(const App &app)
{
    std::vector<std::string> result;
    const std::string query = lower(app.picker_search);
    for (const std::string &candidate : app.candidates)
    {
        const bool already_added = std::any_of(
            app.config.files.begin(), app.config.files.end(),
            [&](const FileVersion &file) { return file.path == candidate; });
        if (!already_added &&
            (query.empty() || lower(candidate).find(query) != std::string::npos))
            result.push_back(candidate);
    }
    return result;
}

void draw_button(const App &app, int x, int y, int width, int height,
                 const std::string &label, bool primary, bool enabled = true)
{
    const Color fill = !enabled ? kPanelLight
                               : primary ? kAccent : kPanelLight;
    rounded_rect(x, y + 2, width, height, 10.0f, kShadow);
    rounded_rect(x, y, width, height, 10.0f, fill);
    if (!primary || !enabled)
        rounded_rect(x, y, width, height, 10.0f,
                     enabled ? kPanelLight : kMuted, true);
    const int approx_width = static_cast<int>(label.size()) * 8;
    text(app, x + (width - approx_width) / 2, y + height / 2 + 5, label,
         enabled && primary ? kPanel : enabled ? kText : kMuted);
}

void draw_shell(const App &app)
{
    rect(0, 0, app.width, app.height, kBackground);
    rect(0, 0, app.width, kHeaderHeight, kPanel);
    rect(0, kHeaderHeight - 1, app.width, 1, kPanelLight);

    const std::array<std::string, 2> tabs = {{"Version", "Config"}};
    const int nav_width = 136;
    const int nav_start = (app.width - static_cast<int>(tabs.size()) * nav_width) / 2;
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        const int x = nav_start + static_cast<int>(i) * nav_width;
        const bool active = (i == 0 && app.tab == Tab::Version) ||
                            (i == 1 && app.tab == Tab::Config);
        if (active)
            rounded_rect(x + 5, 9, nav_width - 10, 56, 12.0f, kAccentWash);
        const Color icon_color = active ? kAccent : kMuted;
        circle(x + nav_width / 2, 26, 6.0f, icon_color);
        if (app.tab == Tab::Version && i == 0)
        {
            rect(x + nav_width / 2 - 7, 37, 14, 2, icon_color);
            rect(x + nav_width / 2 - 5, 42, 10, 2, icon_color);
        }
        else if (app.tab == Tab::Config && i == 1)
        {
            rect(x + nav_width / 2 - 7, 36, 14, 12, icon_color);
            rect(x + nav_width / 2 - 4, 39, 8, 2, kPanel);
        }
        else
        {
            rect(x + nav_width / 2 - 6, 36, 12, 2, icon_color);
            rect(x + nav_width / 2 - 6, 41, 12, 2, icon_color);
            rect(x + nav_width / 2 - 6, 46, 8, 2, icon_color);
        }
        const int label_width = static_cast<int>(tabs[i].size()) * 8;
        text(app, x + (nav_width - label_width) / 2, 61, tabs[i],
             active ? kAccent : kMuted);
    }

    const int content_width = std::min(880, app.width - 64);
    const int left = (app.width - content_width) / 2;
    if (app.tab == Tab::Config)
    {
        text(app, left, 124, "Kernel Build Configuration", kText);
        text(app, left, 150,
             "Choose driver sources to include; disabling dependencies may cause link errors.",
             kMuted);
        if (!app.general_config_error.empty())
        {
            rounded_panel(left, 176, content_width, 134, kPanel, kPanelLight);
            circle(left + 34, 214, 12, kRed);
            text(app, left + 30, 219, "!", kPanel);
            text(app, left + 62, 208, "ERROR", kRed);
            text(app, left + 62, 232, app.general_config_error, kText);
            text(app, left + 62, 260,
                 "Check Kernel/config.cfg and reopen this tab.",
                 kMuted);
            return;
        }

        const size_t enabled_count = static_cast<size_t>(std::count_if(
            app.driver_settings.begin(), app.driver_settings.end(),
            [](const DriverSetting &setting) { return setting.enabled; }));
        rounded_panel(left, 170, content_width, 60, kPanel, kPanelLight);
        text(app, left + 18, 194, "Driver source files", kText);
        text(app, left + 18, 215,
             "Changes are saved to config.cfg immediately.",
             kMuted);
        text(app, left + content_width - 190, 204,
             std::to_string(enabled_count) + " / " +
                 std::to_string(app.driver_settings.size()) + " enabled",
             kAccent);
        text(app, left, 249, "DRIVER", kMuted);

        const int list_bottom = app.height - 70;
        glEnable(GL_SCISSOR_TEST);
        glScissor(left, app.height - list_bottom, content_width,
                  list_bottom - kConfigListTop);
        const int visible_count =
            (list_bottom - kConfigListTop + 6) / kConfigRowHeight;
        for (int visual = 0; visual < visible_count; ++visual)
        {
            const int index = visual + app.config_scroll;
            if (index >= static_cast<int>(app.driver_settings.size()))
                break;
            const DriverSetting &setting =
                app.driver_settings[static_cast<size_t>(index)];
            const int y = kConfigListTop + visual * kConfigRowHeight;
            rounded_rect(left, y + 2, content_width, 36, 9.0f, kShadow);
            rounded_rect(left, y, content_width, 36, 9.0f, kPanel);
            rounded_rect(left, y, content_width, 36, 9.0f, kPanelLight, true);
            text(app, left + 14, y + 23, setting.path, kText);
            draw_button(app, left + content_width - 94, y + 3, 80, 30,
                        setting.enabled ? "Y  ON" : "N  OFF",
                        setting.enabled);
        }
        glDisable(GL_SCISSOR_TEST);

        rect(0, app.height - 62, app.width, 62, kPanel);
        rect(0, app.height - 62, app.width, 1, kPanelLight);
        text(app, left, app.height - 28,
             app.status.empty() ? "Scroll to see all drivers; click a row to toggle."
                                : app.status,
             app.status.empty() ? kMuted : kAccent);
        return;
    }

    text(app, left, 124, "File Versions", kText);
    text(app, left, 150,
         "Manage the version assigned to every tracked Kernel file.", kMuted);
    if (!app.version_config_available)
    {
        rounded_panel(left, 176, content_width, 134, kPanel, kPanelLight);
        circle(left + 34, 214, 12, kRed);
        text(app, left + 30, 219, "!", kPanel);
        text(app, left + 62, 208, "ERROR", kRed);
        text(app, left + 62, 232,
             "ERROR: Version.cfg wasn't found!", kText);
        text(app, left + 62, 260,
             app.config_error.empty()
                 ? "Create Kernel/Version.cfg and reopen the configuration app."
                 : app.config_error,
             kMuted);
        return;
    }

    rounded_panel(left, 169, content_width, 54, kPanel, kPanelLight);
    text(app, left + 18, 191, "Base version", kText);
    text(app, left + 18, 208, "Default for newly added files", kMuted);
    rounded_rect(left + 180, 181, 108, 30, 15.0f, kPanelLight);
    text(app, left + 197, 201,
         app.editing_base ? app.edit_value : app.config.base_version,
         app.editing_base ? kAccent : kText);
    text(app, left + 318, 202, "Kernel release", kMuted);
    text(app, left + 430, 202, app.config.kernel_version, kText);

    const bool selected_missing =
        app.selected >= 0 &&
        app.selected < static_cast<int>(app.config.files.size()) &&
        !file_exists_in_root(app, app.config.files[static_cast<size_t>(app.selected)].path);

    rounded_rect(left, 235, content_width - 156, 38, 19.0f, kPanel);
    rounded_rect(left, 235, content_width - 156, 38, 19.0f,
                 app.editing_search ? kAccent : kPanelLight, true);
    circle(left + 20, 253, 5, kMuted);
    outline(left + 23, 256, 7, 7, kMuted);
    text(app, left + 40, 259,
         app.search.empty() ? "Search files or versions..." : app.search,
         app.search.empty() ? kMuted : kText);
    draw_button(app, left + content_width - 140, 235, 140, 38, "+ Add file", true);

    text(app, left, 289, "KERNEL FILES", kMuted);
    if (selected_missing)
        draw_button(app, left + content_width - 184, 276, 184, 30,
                    "Remove missing entry", false);
    else if (app.selected >= 0 &&
             app.selected < static_cast<int>(app.config.files.size()))
        draw_button(app, left + content_width - 130, 276, 130, 30,
                    "Edit note", false);

    glEnable(GL_SCISSOR_TEST);
    const int list_bottom = app.height - 82;
    glScissor(left, app.height - list_bottom, content_width,
              list_bottom - kListTop);
    const auto rows = visible_rows(app);
    const int count = (list_bottom - kListTop + 12) / kRowHeight;
    for (int visual = 0; visual < count; ++visual)
    {
        const int row_index = visual + app.scroll;
        if (row_index >= static_cast<int>(rows.size()))
            break;
        const size_t config_index = rows[static_cast<size_t>(row_index)];
        const FileVersion &file = app.config.files[config_index];
        const int y = kListTop + visual * kRowHeight;
        const bool selected = app.selected == static_cast<int>(config_index);
        const bool exists = file_exists_in_root(app, file.path);
        rounded_rect(left, y + 3, content_width, 62, 12.0f, kShadow);
        rounded_rect(left, y, content_width, 62, 12.0f,
                     selected ? kAccentWash : kPanel);
        rounded_rect(left, y, content_width, 62, 12.0f,
                     selected ? kAccent : kPanelLight, true);
        circle(left + 28, y + 31, 12,
               exists ? Color{0.91f, 0.96f, 1.0f, 1.0f}
                      : Color{1.0f, 0.93f, 0.93f, 1.0f});
        text(app, left + 23, y + 35, exists ? "f" : "!", exists ? kAccent : kRed);
        text(app, left + 52, y + 28, file.path, exists ? kText : kRed);
        text(app, left + 52, y + 47,
             file.comment.empty() ? "Tracked in Kernel/Version.cfg" : file.comment,
             kMuted);
        if (app.edit_row == static_cast<int>(config_index))
        {
            rounded_rect(left + content_width - 278, y + 16, 104, 31, 15,
                         kPanel);
            rounded_rect(left + content_width - 278, y + 16, 104, 31, 15,
                         kAccent, true);
            text(app, left + content_width - 263, y + 37, app.edit_value, kAccent);
        }
        else
        {
            rounded_rect(left + content_width - 278, y + 16, 104, 31, 15,
                         kPanelLight);
            text(app, left + content_width - 263, y + 37, file.version, kText);
        }
        circle(left + content_width - 34, y + 31, 5, exists ? kGreen : kRed);
    }
    glDisable(GL_SCISSOR_TEST);

    rect(0, app.height - 62, app.width, 62, kPanel);
    rect(0, app.height - 62, app.width, 1, kPanelLight);
    text(app, left, app.height - 28,
         app.status.empty()
             ? (app.selected >= 0 &&
                        app.selected < static_cast<int>(app.config.files.size()) &&
                        !app.config.files[static_cast<size_t>(app.selected)].comment.empty()
                    ? app.config.files[static_cast<size_t>(app.selected)].comment
                    : dirty_config(app) ? "Unsaved changes" : "All changes saved")
             : app.status,
         app.status.empty() ? (dirty_config(app) ? kAccent : kGreen) : kMuted);
    text(app, app.width - 300, app.height - 28, "Click a version to edit", kMuted);
    draw_button(app, app.width - 132, app.height - 48, 108, 34, "Save",
                dirty_config(app), true);
}

void draw_picker(const App &app)
{
    rect(0, 0, app.width, app.height, {0.0f, 0.0f, 0.0f, 0.57f});
    const int x = 170;
    const int y = 90;
    const int width = app.width - 340;
    const int height = app.height - 180;
    rounded_rect(x, y + 5, width, height, 18, kShadow);
    rounded_rect(x, y, width, height, 18, kPanel);
    rounded_rect(x, y, width, height, 18, kPanelLight, true);
    text(app, x + 24, y + 34, "Add a file to Version.cfg", kText);
    text(app, x + 24, y + 59, "Choose a file or type a Kernel-relative path.", kMuted);

    text(app, x + 24, y + 91, "Search", kMuted);
    rounded_rect(x + 92, y + 70, width - 116, 32, 10, kBackground);
    rounded_rect(x + 92, y + 70, width - 116, 32, 10, kPanelLight, true);
    text(app, x + 104, y + 91,
         app.picker_search.empty() ? "Filter available files..." : app.picker_search,
         app.picker_search.empty() ? kMuted : kText);

    text(app, x + 24, y + 129, "Relative path", kMuted);
    rounded_rect(x + 132, y + 108, width - 156, 32, 10, kBackground);
    rounded_rect(x + 132, y + 108, width - 156, 32, 10,
                 app.picker_path_focused ? kAccent : kPanelLight, true);
    text(app, x + 144, y + 129,
         app.picker_path.empty() ? "e.g. Drivers/PCI/pci.c" : app.picker_path,
         app.picker_path.empty() ? kMuted : kText);

    const auto items = filtered_candidates(app);
    const int list_y = y + 154;
    const int list_height = height - 220;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, app.height - (list_y + list_height), width, list_height);
    const int rows = list_height / 30;
    for (int i = 0; i < rows && i + app.picker_scroll < static_cast<int>(items.size()); ++i)
    {
        const int item_index = i + app.picker_scroll;
        const int row_y = list_y + i * 30;
        rect(x + 12, row_y, width - 24, 29,
             item_index == app.picker_selected ? Color{0.13f, 0.25f, 0.37f, 1.0f}
                                               : kPanel);
        text(app, x + 24, row_y + 20, items[static_cast<size_t>(item_index)], kText);
    }
    glDisable(GL_SCISSOR_TEST);
    text(app, x + 20, y + height - 48, app.status, kMuted);
    draw_button(app, x + width - 250, y + height - 52, 104, 32, "Cancel", false);
    draw_button(app, x + width - 132, y + height - 52, 108, 32,
                "Add selected", true, app.picker_selected >= 0 || !app.picker_path.empty());
}

void draw_confirmation(const App &app)
{
    rect(0, 0, app.width, app.height, {0.0f, 0.0f, 0.0f, 0.6f});
    const int width = 460;
    const int height = app.modal == Modal::RemoveConfirmation ? 210 : 230;
    const int x = (app.width - width) / 2;
    const int y = (app.height - height) / 2;
    rounded_rect(x, y + 5, width, height, 18, kShadow);
    rounded_rect(x, y, width, height, 18, kPanel);
    rounded_rect(x, y, width, height, 18, kPanelLight, true);
    const std::string heading =
        app.modal == Modal::RemoveConfirmation ? "Remove this config entry?" : "Save changes?";
    text(app, x + 28, y + 42, heading, kText);
    if (app.modal == Modal::RemoveConfirmation)
    {
        text(app, x + 28, y + 78, "Are you sure you want to delete this entry?", kMuted);
        if (app.selected >= 0 &&
            app.selected < static_cast<int>(app.config.files.size()))
            text(app, x + 28, y + 102,
                 app.config.files[static_cast<size_t>(app.selected)].path, kText);
        text(app, x + 28, y + 128, "The file on disk will not be deleted.", kMuted);
    }
    else
    {
        text(app, x + 28, y + 82, "Update Kernel/Version.cfg with these values?", kMuted);
        text(app, x + 28, y + 108, app.config.files.size() == 1
                                     ? "1 file entry will be written."
                                     : std::to_string(app.config.files.size()) +
                                           " file entries will be written.",
             kMuted);
    }
    draw_button(app, x + width - 238, y + height - 58, 96, 34,
                app.modal == Modal::RemoveConfirmation ? "No" : "Cancel", false);
    draw_button(app, x + width - 128, y + height - 58, 100, 34,
                app.modal == Modal::RemoveConfirmation ? "Yes" : "Save", true);
}

void draw_note_editor(const App &app)
{
    rect(0, 0, app.width, app.height, {0.0f, 0.0f, 0.0f, 0.6f});
    const int width = 560;
    const int height = 220;
    const int x = (app.width - width) / 2;
    const int y = (app.height - height) / 2;
    rounded_rect(x, y + 5, width, height, 18, kShadow);
    rounded_rect(x, y, width, height, 18, kPanel);
    rounded_rect(x, y, width, height, 18, kPanelLight, true);
    text(app, x + 24, y + 38, "File version note", kText);
    if (app.selected >= 0 &&
        app.selected < static_cast<int>(app.config.files.size()))
        text(app, x + 24, y + 65,
             app.config.files[static_cast<size_t>(app.selected)].path, kMuted);
    text(app, x + 24, y + 99,
         "Use a note for 0.0.0 entries to explain why the file is decommissioned.",
         kMuted);
    rounded_rect(x + 24, y + 116, width - 48, 34, 10, kBackground);
    rounded_rect(x + 24, y + 116, width - 48, 34, 10, kAccent, true);
    text(app, x + 36, y + 138,
         app.note_value.empty() ? "Type a short explanation..." : app.note_value,
         app.note_value.empty() ? kMuted : kText);
    draw_button(app, x + width - 250, y + height - 52, 104, 32, "Cancel", false);
    draw_button(app, x + width - 132, y + height - 52, 108, 32, "Apply", true);
}

void render(App &app)
{
    glViewport(0, 0, app.width, app.height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, app.width, app.height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(kBackground.r, kBackground.g, kBackground.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_shell(app);
    if (app.modal == Modal::AddFile)
        draw_picker(app);
    else if (app.modal == Modal::SaveConfirmation ||
             app.modal == Modal::RemoveConfirmation)
        draw_confirmation(app);
    else if (app.modal == Modal::EditNote)
        draw_note_editor(app);
    glXSwapBuffers(app.display, app.window);
}

void set_status(App &app, const std::string &message)
{
    app.status = message;
}

bool add_file(App &app, const std::string &candidate)
{
    fs::path safe;
    if (!is_safe_relative_path(candidate, safe))
    {
        set_status(app, "Use a safe, Kernel-relative path outside build/.");
        return false;
    }
    const std::string normalized = safe.generic_string();
    if (!file_exists_in_root(app, normalized))
    {
        set_status(app, "That path is not an existing file under Kernel/.");
        return false;
    }
    if (std::any_of(app.config.files.begin(), app.config.files.end(),
                    [&](const FileVersion &file) { return file.path == normalized; }))
    {
        set_status(app, "That file is already listed.");
        return false;
    }
    app.config.files.push_back({normalized, app.config.base_version, {}});
    std::sort(app.config.files.begin(), app.config.files.end(),
              [](const FileVersion &a, const FileVersion &b) {
                  return a.path < b.path;
              });
    app.selected = static_cast<int>(std::distance(
        app.config.files.begin(),
        std::find_if(app.config.files.begin(), app.config.files.end(),
                     [&](const FileVersion &file) { return file.path == normalized; })));
    app.editing_search = false;
    app.search.clear();
    set_status(app, "Added " + normalized + " at version " + app.config.base_version);
    return true;
}

void commit_version_edit(App &app)
{
    if (app.editing_base)
    {
        if (valid_version(app.edit_value))
        {
            app.config.base_version = app.edit_value;
            set_status(app, "Base version updated.");
        }
        else
            set_status(app, "Version must use x.y.z format.");
        app.editing_base = false;
        app.edit_value.clear();
    }
    else if (app.edit_row >= 0 && app.edit_row < static_cast<int>(app.config.files.size()))
    {
        if (valid_version(app.edit_value))
        {
            app.config.files[static_cast<size_t>(app.edit_row)].version = app.edit_value;
            set_status(app, "File version updated.");
        }
        else
            set_status(app, "Version must use x.y.z format.");
        app.edit_row = -1;
        app.edit_value.clear();
    }
}

void open_picker(App &app)
{
    app.modal = Modal::AddFile;
    app.picker_search.clear();
    app.picker_path.clear();
    app.picker_selected = -1;
    app.picker_scroll = 0;
    app.picker_path_focused = false;
    app.candidates = discover_files(app);
    set_status(app, "Select a file or type its relative path.");
}

void add_picker_selection(App &app)
{
    std::string candidate = app.picker_path;
    if (candidate.empty())
    {
        const auto items = filtered_candidates(app);
        if (app.picker_selected < 0 ||
            app.picker_selected >= static_cast<int>(items.size()))
            return;
        candidate = items[static_cast<size_t>(app.picker_selected)];
    }
    if (add_file(app, candidate))
        app.modal = Modal::Idle;
}

void save_confirmed(App &app)
{
    std::string error;
    if (!save_config(app, error))
    {
        app.modal = Modal::Idle;
        app.exit_after_save = false;
        set_status(app, error);
        return;
    }
    app.saved_config = app.config;
    app.modal = Modal::Idle;
    set_status(app, "Version.cfg saved.");
}

void remove_confirmed(App &app)
{
    if (app.selected >= 0 &&
        app.selected < static_cast<int>(app.config.files.size()))
    {
        const std::string removed =
            app.config.files[static_cast<size_t>(app.selected)].path;
        app.config.files.erase(app.config.files.begin() + app.selected);
        app.selected = -1;
        app.edit_row = -1;
        set_status(app, "Removed " + removed + " from config. Save to apply.");
    }
    app.modal = Modal::Idle;
}

void begin_edit_row(App &app, int row)
{
    if (row < 0 || row >= static_cast<int>(app.config.files.size()))
        return;
    app.selected = row;
    app.edit_row = row;
    app.editing_base = false;
    app.editing_search = false;
    app.edit_value = app.config.files[static_cast<size_t>(row)].version;
}

void toggle_driver(App &app, int index)
{
    if (index < 0 || index >= static_cast<int>(app.driver_settings.size()))
        return;
    DriverSetting updated = app.driver_settings[static_cast<size_t>(index)];
    updated.enabled = !updated.enabled;
    std::string error;
    if (!save_driver_setting(app.general_config_path, updated, error))
    {
        set_status(app, error);
        return;
    }
    app.driver_settings[static_cast<size_t>(index)] = std::move(updated);
    set_status(app, "Saved driver selection to config.cfg.");
}

void handle_main_click(App &app, int x, int y)
{
    const int content_width = std::min(880, app.width - 64);
    const int content_left = (app.width - content_width) / 2;
    const bool same_base_cell = app.editing_base && y >= 169 && y <= 223 &&
                                x >= content_left + 180 &&
                                x < content_left + 288;
    bool same_version_cell = false;
    if (app.edit_row >= 0 && y >= kListTop && y < app.height - 82 &&
        x >= content_left + content_width - 286 &&
        x < content_left + content_width - 174)
    {
        const int visual = (y - kListTop) / kRowHeight;
        const auto rows = visible_rows(app);
        const int row = visual + app.scroll;
        same_version_cell = row >= 0 && row < static_cast<int>(rows.size()) &&
                            static_cast<int>(rows[static_cast<size_t>(row)]) ==
                                app.edit_row;
    }
    if ((app.edit_row >= 0 || app.editing_base) &&
        !same_version_cell && !same_base_cell)
        commit_version_edit(app);

    const bool clicked_search = y >= 235 && y <= 273 &&
                                x >= content_left &&
                                x < content_left + content_width - 156;
    if (app.editing_search && !clicked_search)
        app.editing_search = false;

    if (y < kHeaderHeight)
    {
        const int nav_width = 136;
        const int nav_start = (app.width - 2 * nav_width) / 2;
        if (x >= nav_start && x < nav_start + nav_width)
        {
            if (app.edit_row >= 0 || app.editing_base)
                commit_version_edit(app);
            app.editing_search = false;
            app.tab = Tab::Version;
        }
        else if (x >= nav_start + nav_width && x < nav_start + 2 * nav_width)
        {
            if (app.edit_row >= 0 || app.editing_base)
                commit_version_edit(app);
            app.editing_search = false;
            app.tab = Tab::Config;
        }
        return;
    }
    if (app.tab == Tab::Config)
    {
        if (app.general_config_error.empty() &&
            y >= kConfigListTop && y < app.height - 70)
        {
            const int visual = (y - kConfigListTop) / kConfigRowHeight;
            const int index = visual + app.config_scroll;
            if (index >= 0 &&
                index < static_cast<int>(app.driver_settings.size()))
                toggle_driver(app, index);
        }
        return;
    }
    if (app.tab != Tab::Version || !app.version_config_available)
        return;

    if (y >= app.height - 62 && x >= app.width - 132 &&
        app.tab == Tab::Version && app.version_config_available)
    {
        if (app.edit_row >= 0 || app.editing_base)
            commit_version_edit(app);
        app.editing_search = false;
        app.modal = Modal::SaveConfirmation;
        return;
    }
    if (y >= 276 && y <= 306 &&
        x >= content_left + content_width - 130 &&
        x < content_left + content_width &&
        app.selected >= 0 &&
        app.selected < static_cast<int>(app.config.files.size()))
    {
        app.note_value = app.config.files[static_cast<size_t>(app.selected)].comment;
        app.modal = Modal::EditNote;
        return;
    }
    if (y >= kListTop && y < app.height - 82 &&
        x >= content_left && x < content_left + content_width)
    {
        app.editing_search = false;
        const int visual = (y - kListTop) / kRowHeight;
        const auto rows = visible_rows(app);
        const int row = visual + app.scroll;
        if (row >= 0 && row < static_cast<int>(rows.size()))
        {
            const int index = static_cast<int>(rows[static_cast<size_t>(row)]);
            app.selected = index;
            if (x >= content_left + content_width - 286 &&
                x < content_left + content_width - 174)
            {
                if (app.edit_row != index)
                    begin_edit_row(app, index);
            }
        }
        return;
    }
    if (y >= 169 && y <= 223 &&
        x >= content_left + 180 && x < content_left + 288)
    {
        if (!app.editing_base)
        {
            app.editing_base = true;
            app.edit_row = -1;
            app.edit_value = app.config.base_version;
        }
        return;
    }
    if (y >= 235 && y <= 273 && x >= content_left &&
        x < content_left + content_width - 156)
    {
        app.editing_search = true;
        app.editing_base = false;
        app.edit_row = -1;
        app.edit_value.clear();
        return;
    }
    if (y >= 235 && y <= 273 &&
        x >= content_left + content_width - 140 &&
        x <= content_left + content_width)
    {
        open_picker(app);
        return;
    }
    if (app.selected >= 0 && app.selected < static_cast<int>(app.config.files.size()) &&
        !file_exists_in_root(app, app.config.files[static_cast<size_t>(app.selected)].path))
    {
        if (y >= 276 && y <= 306 &&
            x >= content_left + content_width - 184)
        {
            app.modal = Modal::RemoveConfirmation;
            return;
        }
    }
}

void handle_file_right_click(App &app, int x, int y)
{
    if (app.tab != Tab::Version || !app.version_config_available ||
        y < kListTop || y >= app.height - 82)
        return;
    const int content_width = std::min(880, app.width - 64);
    const int content_left = (app.width - content_width) / 2;
    if (x < content_left || x >= content_left + content_width)
        return;
    if (app.edit_row >= 0 || app.editing_base)
        commit_version_edit(app);
    const int visual = (y - kListTop) / kRowHeight;
    const auto rows = visible_rows(app);
    const int row = visual + app.scroll;
    if (row < 0 || row >= static_cast<int>(rows.size()))
        return;
    app.selected = static_cast<int>(rows[static_cast<size_t>(row)]);
    app.editing_search = false;
    app.edit_row = -1;
    app.modal = Modal::RemoveConfirmation;
}

void handle_picker_click(App &app, int x, int y)
{
    const int modal_x = 170;
    const int modal_y = 90;
    const int modal_width = app.width - 340;
    const int modal_height = app.height - 180;
    const int list_y = modal_y + 154;
    const int list_height = modal_height - 220;
    if (y >= modal_y + 70 && y <= modal_y + 102)
    {
        app.picker_path_focused = false;
        app.editing_search = false;
        app.picker_selected = -1;
        app.picker_path.clear();
        return;
    }
    if (y >= modal_y + 108 && y <= modal_y + 140)
    {
        app.picker_path_focused = true;
        app.picker_path.clear();
        app.picker_selected = -1;
        return;
    }
    if (y >= list_y && y < list_y + list_height)
    {
        const int visual = (y - list_y) / 30;
        const int index = visual + app.picker_scroll;
        const auto items = filtered_candidates(app);
        if (index >= 0 && index < static_cast<int>(items.size()))
        {
            app.picker_selected = index;
            app.picker_path = items[static_cast<size_t>(index)];
        }
        return;
    }
    if (y >= modal_y + modal_height - 58)
    {
        if (x >= modal_x + modal_width - 250 && x < modal_x + modal_width - 142)
            app.modal = Modal::Idle;
        else if (x >= modal_x + modal_width - 142 &&
                 x < modal_x + modal_width - 12)
            add_picker_selection(app);
    }
}

void handle_confirmation_click(App &app, int x, int y)
{
    const int width = 460;
    const int height = app.modal == Modal::RemoveConfirmation ? 210 : 230;
    const int left = (app.width - width) / 2;
    const int top = (app.height - height) / 2;
    if (y < top + height - 58 || y > top + height - 20)
        return;
    if (x >= left + width - 238 && x < left + width - 132)
    {
        app.modal = Modal::Idle;
        app.exit_after_save = false;
    }
    else if (x >= left + width - 128 && x < left + width - 28)
    {
        if (app.modal == Modal::SaveConfirmation)
            save_confirmed(app);
        else
            remove_confirmed(app);
    }
}

void handle_note_click(App &app, int x, int y)
{
    const int width = 560;
    const int height = 220;
    const int left = (app.width - width) / 2;
    const int top = (app.height - height) / 2;
    if (y < top + height - 52 || y > top + height - 20)
        return;
    if (x >= left + width - 250 && x < left + width - 142)
    {
        app.modal = Modal::Idle;
    }
    else if (x >= left + width - 132 && x < left + width - 24 &&
             app.selected >= 0 &&
             app.selected < static_cast<int>(app.config.files.size()))
    {
        app.config.files[static_cast<size_t>(app.selected)].comment = app.note_value;
        app.modal = Modal::Idle;
        set_status(app, "File note updated.");
    }
}

void handle_key(App &app, XKeyEvent &event)
{
    char buffer[32]{};
    KeySym symbol = NoSymbol;
    const int length = XLookupString(&event, buffer, sizeof(buffer), &symbol, nullptr);
    if (symbol == XK_Escape)
    {
        if (app.modal != Modal::Idle)
        {
            app.modal = Modal::Idle;
            app.exit_after_save = false;
        }
        else
        {
            app.edit_row = -1;
            app.editing_base = false;
            app.editing_search = false;
            app.edit_value.clear();
        }
        return;
    }

    const bool control = (event.state & ControlMask) != 0;
    if (control && (symbol == XK_s || symbol == XK_S) &&
        app.modal == Modal::Idle)
    {
        if (app.edit_row >= 0 || app.editing_base)
            commit_version_edit(app);
        app.editing_search = false;
        app.modal = Modal::SaveConfirmation;
        return;
    }

    if (app.modal == Modal::SaveConfirmation ||
        app.modal == Modal::RemoveConfirmation)
    {
        if (symbol == XK_Return || symbol == XK_KP_Enter)
        {
            if (app.modal == Modal::SaveConfirmation)
                save_confirmed(app);
            else
                remove_confirmed(app);
        }
        return;
    }
    if (app.modal == Modal::EditNote)
    {
        if (symbol == XK_Return || symbol == XK_KP_Enter)
        {
            if (app.selected >= 0 &&
                app.selected < static_cast<int>(app.config.files.size()))
            {
                app.config.files[static_cast<size_t>(app.selected)].comment =
                    app.note_value;
                set_status(app, "File note updated.");
            }
            app.modal = Modal::Idle;
            return;
        }
        if (symbol == XK_BackSpace)
        {
            if (!app.note_value.empty())
                app.note_value.pop_back();
            return;
        }
        for (int i = 0; i < length; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(buffer[i]);
            if (ch >= 32 && ch < 127)
                app.note_value.push_back(static_cast<char>(ch));
        }
        return;
    }
    if (app.modal == Modal::AddFile)
    {
        std::string *target = app.picker_path_focused
                                  ? &app.picker_path
                                  : &app.picker_search;
        if (symbol == XK_Return || symbol == XK_KP_Enter)
        {
            if (app.picker_path_focused)
                add_picker_selection(app);
            else
            {
                app.picker_selected = -1;
                app.picker_scroll = 0;
            }
            return;
        }
        if (symbol == XK_BackSpace)
        {
            if (!target->empty())
                target->pop_back();
            if (app.editing_search)
                app.scroll = 0;
            return;
        }
        if (!app.picker_path_focused && symbol == XK_Tab)
        {
            app.picker_path_focused = true;
            return;
        }
        if (!app.picker_path_focused)
        {
            app.picker_selected = -1;
            app.picker_scroll = 0;
            app.picker_path.clear();
        }
        for (int i = 0; i < length; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(buffer[i]);
            if (ch >= 32 && ch < 127)
                target->push_back(static_cast<char>(ch));
        }
        if (app.editing_search)
            app.scroll = 0;
        return;
    }

    if (app.edit_row >= 0 || app.editing_base || app.editing_search)
    {
        std::string *target = app.editing_search ? &app.search : &app.edit_value;
        if (symbol == XK_Return || symbol == XK_KP_Enter)
        {
            if (app.editing_search)
                app.editing_search = false;
            else
                commit_version_edit(app);
            return;
        }
        if (symbol == XK_BackSpace)
        {
            if (!target->empty())
                target->pop_back();
            return;
        }
        if (app.editing_search && symbol == XK_Escape)
        {
            app.search.clear();
            app.editing_search = false;
            return;
        }
        for (int i = 0; i < length; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(buffer[i]);
            if (ch >= 32 && ch < 127)
                target->push_back(static_cast<char>(ch));
        }
        return;
    }

    if (symbol == XK_Delete && app.selected >= 0 &&
        app.selected < static_cast<int>(app.config.files.size()) &&
        !file_exists_in_root(app, app.config.files[static_cast<size_t>(app.selected)].path))
        app.modal = Modal::RemoveConfirmation;
}

int run_check(const fs::path &root)
{
    App app;
    app.root = root;
    app.config_path = root / "Kernel" / "Version.cfg";
    std::string error;
    if (!load_config(app.config_path, app.config, error))
    {
        std::cerr << error << '\n';
        return 1;
    }

    size_t missing = 0;
    size_t invalid = 0;
    for (const FileVersion &file : app.config.files)
    {
        if (!file_exists_in_root(app, file.path))
        {
            std::cerr << "Missing: " << file.path << '\n';
            ++missing;
        }
        if (!valid_version(file.version) ||
            (file.version == "0.0.0" && file.comment.empty()))
            ++invalid;
    }
    std::cout << "Version.cfg parsed: " << app.config.files.size()
              << " file entries; base " << app.config.base_version
              << "; kernel " << app.config.kernel_version << ".\n";
    if (missing || invalid)
    {
        std::cerr << missing << " missing file(s), " << invalid
                  << " invalid version(s).\n";
        return 1;
    }
    std::cout << "All listed files exist and have valid versions.\n";
    std::vector<DriverSetting> drivers;
    if (!load_build_config(root / "Kernel" / "config.cfg", drivers, error))
    {
        std::cerr << "Build config error: " << error << '\n';
        return 1;
    }
    const size_t enabled = static_cast<size_t>(std::count_if(
        drivers.begin(), drivers.end(),
        [](const DriverSetting &setting) { return setting.enabled; }));
    std::cout << "config.cfg parsed: " << enabled << " of " << drivers.size()
              << " drivers enabled.\n";
    return 0;
}

int run_save_self_test()
{
    const std::string pattern =
        (fs::temp_directory_path() / "versitile-config-test-XXXXXX").string();
    std::vector<char> temp_template(pattern.begin(), pattern.end());
    temp_template.push_back('\0');
    char *created = mkdtemp(temp_template.data());
    if (!created)
    {
        std::cerr << "Could not create temporary config test directory.\n";
        return 1;
    }

    const fs::path root(created);
    const fs::path kernel = root / "Kernel";
    std::error_code ec;
    fs::create_directories(kernel, ec);
    if (ec)
    {
        std::cerr << "Could not set up temporary config test.\n";
        fs::remove_all(root, ec);
        return 1;
    }
    {
        std::ofstream source(kernel / "sample.c");
        source << "int sample;\n";
        std::ofstream config(kernel / "Version.cfg");
        config << "kernel_version = 5.0.7\n"
               << "base_version = 1.0.1\n\n"
               << "[files]\n"
               << "sample.c = 1.0.1\n";
        std::ofstream build_config(kernel / "config.cfg");
        build_config << "CC = gcc\n"
                     << "DRIVER_ENABLE_Drivers/Test/sample.c = y\n";
        if (!source || !config || !build_config)
        {
            std::cerr << "Could not write temporary config test fixtures.\n";
            fs::remove_all(root, ec);
            return 1;
        }
    }

    App app;
    app.root = root;
    app.config_path = kernel / "Version.cfg";
    app.general_config_path = kernel / "config.cfg";
    std::string error;
    bool passed = load_config(app.config_path, app.config, error);
    if (passed)
    {
        app.config.base_version = "2.0.0";
        app.config.files[0].version = "2.1.3";
        app.config.files[0].comment = "updated by self-test";
        passed = save_config(app, error);
    }

    VersionConfig reloaded;
    if (passed)
        passed = load_config(app.config_path, reloaded, error);
    passed = passed && reloaded.base_version == "2.0.0" &&
             reloaded.files.size() == 1 &&
             reloaded.files[0].version == "2.1.3" &&
             reloaded.files[0].comment == "updated by self-test";
    if (passed)
    {
        app.config = reloaded;
        app.version_config_available = true;
        passed = load_build_config(app.general_config_path,
                                   app.driver_settings, error) &&
                 app.driver_settings.size() == 1 &&
                 app.driver_settings[0].enabled;
    }
    if (passed)
    {
        DriverSetting disabled = app.driver_settings[0];
        disabled.enabled = false;
        passed = save_driver_setting(app.general_config_path, disabled, error);
        std::vector<DriverSetting> reloaded_drivers;
        if (passed)
            passed = load_build_config(app.general_config_path,
                                       reloaded_drivers, error) &&
                     reloaded_drivers.size() == 1 &&
                     !reloaded_drivers[0].enabled;
    }
    if (passed)
    {
        app.search = "sample";
        app.editing_search = true;
        const int content_width = std::min(880, app.width - 64);
        const int content_left = (app.width - content_width) / 2;
        handle_main_click(app, content_left + 630, kListTop + 10);
        passed = !app.editing_search && app.edit_row == 0;
        app.edit_value = "2.2.2";
        handle_file_right_click(app, app.width / 2, kListTop + 10);
        passed = passed && app.modal == Modal::RemoveConfirmation &&
                 app.selected == 0 &&
                 app.config.files[0].version == "2.2.2";
        if (passed)
            remove_confirmed(app);
        passed = passed && app.config.files.empty() &&
                 fs::is_regular_file(kernel / "sample.c") &&
                 fs::is_regular_file(app.general_config_path);
    }
    fs::remove_all(root, ec);
    if (!passed)
    {
        std::cerr << "Version.cfg save/reload test failed: " << error << '\n';
        return 1;
    }
    std::cout << "Version.cfg save/reload, driver toggle, and removal tests passed.\n";
    return 0;
}

bool initialize_window(App &app)
{
    app.display = XOpenDisplay(nullptr);
    if (!app.display)
    {
        std::cerr << "Cannot open X display. Set DISPLAY and try again.\n";
        return false;
    }

    const int screen = DefaultScreen(app.display);
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                        GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *visual = glXChooseVisual(app.display, screen, attributes);
    if (!visual)
    {
        std::cerr << "No compatible GLX visual found.\n";
        return false;
    }

    app.colormap = XCreateColormap(app.display, RootWindow(app.display, visual->screen),
                                   visual->visual, AllocNone);
    XSetWindowAttributes window_attributes{};
    window_attributes.colormap = app.colormap;
    window_attributes.event_mask = ExposureMask | StructureNotifyMask |
                                   ButtonPressMask | ButtonReleaseMask |
                                   PointerMotionMask | KeyPressMask;
    app.window = XCreateWindow(
        app.display, RootWindow(app.display, visual->screen), 100, 100,
        static_cast<unsigned>(app.width), static_cast<unsigned>(app.height), 0,
        visual->depth, InputOutput, visual->visual,
        CWColormap | CWEventMask, &window_attributes);
    XStoreName(app.display, app.window, "VeRsitileOS Kernel Configuration");
    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width = 820;
    hints.min_height = 600;
    XSetWMNormalHints(app.display, app.window, &hints);
    XMapWindow(app.display, app.window);
    app.context = glXCreateContext(app.display, visual, nullptr, True);
    XFree(visual);
    if (!app.context || !glXMakeCurrent(app.display, app.window, app.context))
    {
        std::cerr << "Could not create an OpenGL context.\n";
        return false;
    }

    app.font = XLoadQueryFont(
        app.display, "-urw-nimbus sans l-regular-r-normal--0-0-0-0-p-0-iso8859-1");
    if (!app.font)
        app.font = XLoadQueryFont(app.display, "fixed");
    if (app.font)
    {
        app.font_lists = glGenLists(256);
        glXUseXFont(app.font->fid, 0, 256, static_cast<int>(app.font_lists));
    }
    return true;
}

void shutdown_window(App &app)
{
    if (app.display)
    {
        if (app.context)
        {
            glXMakeCurrent(app.display, None, nullptr);
            glXDestroyContext(app.display, app.context);
        }
        if (app.font)
            XFreeFont(app.display, app.font);
        if (app.window)
            XDestroyWindow(app.display, app.window);
        if (app.colormap)
            XFreeColormap(app.display, app.colormap);
        XCloseDisplay(app.display);
    }
}

int run_ui(App &app)
{
    if (!initialize_window(app))
    {
        shutdown_window(app);
        return 1;
    }

    Atom wm_delete = XInternAtom(app.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.display, app.window, &wm_delete, 1);
    bool running = true;
    render(app);
    while (running)
    {
        pollfd display_fd{ConnectionNumber(app.display), POLLIN, 0};
        const int poll_result = poll(&display_fd, 1, -1);
        if (poll_result < 0)
        {
            if (errno == EINTR)
                continue;
            std::cerr << "X11 event wait failed.\n";
            break;
        }
        while (XPending(app.display))
        {
            XEvent event{};
            XNextEvent(app.display, &event);
            switch (event.type)
            {
            case ConfigureNotify:
                app.width = event.xconfigure.width;
                app.height = event.xconfigure.height;
                break;
            case Expose:
                break;
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) ==
                    XInternAtom(app.display, "WM_DELETE_WINDOW", False))
                {
                    if (dirty_config(app))
                    {
                        app.modal = Modal::SaveConfirmation;
                        app.exit_after_save = true;
                    }
                    else
                        running = false;
                }
                break;
            case ButtonPress:
                if (event.xbutton.button == Button4 || event.xbutton.button == Button5)
                {
                    const int direction = event.xbutton.button == Button4 ? -1 : 1;
                    if (app.modal == Modal::AddFile)
                        app.picker_scroll = std::max(0, app.picker_scroll + direction * 3);
                    else if (app.tab == Tab::Config)
                    {
                        const int visible_count = std::max(
                            1, (app.height - 70 - kConfigListTop + 6) /
                                   kConfigRowHeight);
                        const int max_scroll = std::max(
                            0, static_cast<int>(app.driver_settings.size()) -
                                   visible_count);
                        app.config_scroll = std::clamp(
                            app.config_scroll + direction * 3, 0, max_scroll);
                    }
                    else
                        app.scroll = std::max(0, app.scroll + direction * 3);
                    break;
                }
                if (event.xbutton.button == Button3)
                {
                    if (app.modal == Modal::Idle)
                        handle_file_right_click(app, event.xbutton.x, event.xbutton.y);
                    break;
                }
                if (event.xbutton.button != Button1)
                    break;
                if (app.modal == Modal::AddFile)
                    handle_picker_click(app, event.xbutton.x, event.xbutton.y);
                else if (app.modal == Modal::EditNote)
                    handle_note_click(app, event.xbutton.x, event.xbutton.y);
                else if (app.modal == Modal::SaveConfirmation ||
                         app.modal == Modal::RemoveConfirmation)
                    handle_confirmation_click(app, event.xbutton.x, event.xbutton.y);
                else
                    handle_main_click(app, event.xbutton.x, event.xbutton.y);
                break;
            case KeyPress:
                handle_key(app, event.xkey);
                break;
            default:
                break;
            }
        }
        if (app.exit_after_save && !dirty_config(app))
            running = false;
        render(app);
        XFlush(app.display);
    }
    shutdown_window(app);
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    fs::path start = fs::current_path();
    const std::string first_argument = argc > 1 ? argv[1] : "";
    if (!first_argument.empty() && first_argument != "--check" &&
        first_argument != "--self-test")
        start = argv[1];
    const auto root = find_root(start);
    if (!root)
    {
        std::cerr << "Cannot find the Kernel directory. Run from the repository "
                     "or pass the repository root.\n";
        return 1;
    }

    App app;
    app.root = *root;
    app.config_path = app.root / "Kernel" / "Version.cfg";
    app.general_config_path = app.root / "Kernel" / "config.cfg";
    if (first_argument == "--check")
        return run_check(*root);
    if (first_argument == "--self-test")
        return run_save_self_test();

    std::string error;
    app.version_config_available = load_config(app.config_path, app.config, error);
    if (!app.version_config_available)
    {
        app.config_error = fs::exists(app.config_path)
                               ? "Version.cfg error: " + error
                               : "ERROR: Version.cfg wasn't found!";
        app.status = app.config_error;
    }
    if (!load_build_config(app.general_config_path, app.driver_settings, error))
    {
        app.general_config_error = fs::exists(app.general_config_path)
                                       ? "config.cfg error: " + error
                                       : "ERROR: config.cfg wasn't found!";
    }
    app.saved_config = app.config;
    return run_ui(app);
}
