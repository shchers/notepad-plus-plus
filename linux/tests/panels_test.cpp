// Phase 4 core tests. The Function List parser runs on upstream's own
// fixtures (PowerEditor/Test/FunctionList/<lang>/unitTest) and must produce
// the same tree, compared through upstream's JSON export
// (FunctionListPanel::serialize, nlohmann::json's compact sorted output).

#include "core/FileManager.h"
#include "core/FunctionParser.h"
#include "core/Session.h"
#include "core/Workspace.h"
#include "platform/FileIO.h"
#include "platform/Sandbox.h"
#include "ui/EditorView.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace npp;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

namespace {

fs::path g_dir;

std::string readText(const fs::path& p)
{
    std::string s;
    platform::readFile(p, s, nullptr);
    return s;
}

// nlohmann::json string escaping (dump() defaults: UTF-8 kept as is).
std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string jsonArray(const std::vector<std::string>& v)
{
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i)
        out += (i ? "," : "") + jsonString(v[i]);
    return out + "]";
}

// Upstream FunctionListPanel::serialize.
std::string exportJson(const std::vector<core::FunctionInfo>& infos, const std::string& root)
{
    std::vector<std::string> leaves;
    std::vector<std::pair<std::string, std::vector<std::string>>> nodes;
    for (const core::FunctionInfo& fi : infos) {
        if (!fi.className.empty()) {
            auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& n) { return n.first == fi.className; });
            if (it == nodes.end())
                nodes.push_back({fi.className, {fi.name}});
            else
                it->second.push_back(fi.name);
        } else {
            leaves.push_back(fi.name);
        }
    }
    std::string out = "{";
    bool first = true;
    if (!leaves.empty()) {
        out += "\"leaves\":" + jsonArray(leaves);
        first = false;
    }
    if (!nodes.empty()) {
        out += std::string(first ? "" : ",") + "\"nodes\":[";
        for (std::size_t i = 0; i < nodes.size(); ++i)
            out += std::string(i ? "," : "") + "{\"leaves\":" + jsonArray(nodes[i].second) + ",\"name\":" + jsonString(nodes[i].first) + "}";
        out += "]";
        first = false;
    }
    out += std::string(first ? "" : ",") + "\"root\":" + jsonString(root) + "}";
    return out;
}

std::string normalise(std::string s)
{
    for (std::size_t p = s.find("\\r\\n"); p != std::string::npos; p = s.find("\\r\\n", p))
        s.replace(p, 4, "\\n");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

void testFunctionListFixtures()
{
    const fs::path root = fs::path(NPP_SOURCE_ROOT) / "PowerEditor" / "Test" / "FunctionList";
    core::FunctionParsers parsers;
    // As upstream's CI: the test-only overrideMap.xml and regexGlobalTest.xml
    // (filesForTesting/) on top of the shipped rules.
    parsers.init(fs::path(NPP_SOURCE_ROOT) / "PowerEditor" / "installer" / "filesForTesting", fs::path(NPP_TEST_DATA_DIR) / "functionList");
    int run = 0, passed = 0;
    for (const auto& langDir : fs::directory_iterator(root)) {
        if (!langDir.is_directory())
            continue;
        const std::string dirName = langDir.path().filename().string();
        std::vector<fs::path> cases = {langDir.path()};
        for (const auto& sub : fs::directory_iterator(langDir.path()))
            if (sub.is_directory())
                cases.push_back(sub.path());
        for (const fs::path& c : cases) {
            if (!fs::exists(c / "unitTest") || !fs::exists(c / "unitTest.expected.result"))
                continue;
            ++run;
            std::string err;
            auto lf = core::loadFile(c / "unitTest", -1, &err);
            // Upstream's CI checks the fixtures out on Windows, with CRLF line
            // endings, and the expected results come from there.
            std::string text = lf ? lf->text.utf8 : "";
            std::string crlf;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
                    crlf += '\r';
                crlf += text[i];
            }
            ui::EditorView v;
            v.setText(crlf);
            std::vector<core::FunctionInfo> infos;
            const bool udl = dirName.rfind("udl-", 0) == 0;
            const bool ok = udl ? parsers.parse(v.sci(), "", dirName.substr(4), infos) : parsers.parse(v.sci(), dirName, "", infos);
            const std::string got = normalise(exportJson(infos, "unitTest"));
            const std::string want = normalise(readText(c / "unitTest.expected.result"));
            if (ok && got == want) {
                ++passed;
            } else {
                ++g_failures;
                std::fprintf(stderr, "FAIL function list %s (%s)\n  want %.300s\n  got  %.300s\n", fs::relative(c, root).c_str(),
                             ok ? "parsed" : "no parser", want.c_str(), got.c_str());
            }
            ++g_checks;
        }
    }
    std::printf("  %d of %d upstream function list fixtures match\n", passed, run);
    CHECK(run >= 40);
}

void writeText(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

void testWorkspace()
{
    // A workspace written on Windows: relative names with backslashes.
    const fs::path wsFile = g_dir / "ws" / "my.workspace";
    writeText(wsFile, "<NotepadPlus>\n"
                      "    <Project name=\"App\">\n"
                      "        <Folder name=\"src\">\n"
                      "            <File name=\"src\\main.cpp\" />\n"
                      "            <Folder name=\"empty\" />\n"
                      "        </Folder>\n"
                      "        <File name=\"/etc/hostname\" />\n"
                      "    </Project>\n"
                      "    <Project name=\"Docs\" />\n"
                      "</NotepadPlus>\n");
    core::Workspace ws;
    std::string err;
    CHECK(core::loadWorkspace(wsFile, ws, &err));
    CHECK(ws.projects.size() == 2 && ws.projects[0].name == "App" && ws.projects[1].children.empty());
    const auto files = core::workspaceFiles(ws);
    CHECK(files.size() == 2 && files[0] == g_dir / "ws" / "src" / "main.cpp" && files[1] == "/etc/hostname");
    CHECK(ws.projects[0].children[0].kind == core::WorkspaceNode::Kind::Folder && ws.projects[0].children[0].children[0].name == "main.cpp");
    // Saved relative to the workspace where possible, as upstream; round trip.
    const fs::path out = g_dir / "ws" / "copy.workspace";
    CHECK(core::saveWorkspace(out, ws, &err));
    const std::string xml = readText(out);
    CHECK(xml.find("<File name=\"src/main.cpp\" />") != std::string::npos && xml.find("<File name=\"/etc/hostname\" />") != std::string::npos);
    CHECK(xml.rfind("<?xml", 0) == std::string::npos);
    core::Workspace again;
    CHECK(core::loadWorkspace(out, again) && again == ws);
    CHECK(!core::loadWorkspace(g_dir / "ws" / "missing.workspace", again, &err) && !err.empty());
}

void testSessionFileBrowser()
{
    core::Session s;
    s.includeFileBrowser = true;
    s.fileBrowserSelected = "/home/u/proj/src/a.cpp";
    s.fileBrowserRoots = {{"/home/u/proj", {"/home/u/proj/src"}}, {"/tmp", {}}};
    const fs::path f = g_dir / "session.xml";
    std::string err;
    CHECK(core::writeSession(f, s, &err));
    CHECK(readText(f).find("<FileBrowser latestSelectedItem=\"/home/u/proj/src/a.cpp\">") != std::string::npos);
    core::Session r;
    CHECK(core::readSession(f, r, &err) && r == s);
    s.includeFileBrowser = false;
    CHECK(core::writeSession(f, s, &err) && readText(f).find("FileBrowser") == std::string::npos);
}

void testSandbox()
{
    using platform::Sandbox;
    const std::vector<std::string> cmd = {"make", "-j4"};
    CHECK(platform::hostCommand(cmd, "/src", Sandbox::None) == cmd);
    CHECK(platform::hostCommand(cmd, "/src", Sandbox::Snap) == cmd);
    CHECK(platform::hostCommand(cmd, "/src", Sandbox::Flatpak) ==
          (std::vector<std::string>{"flatpak-spawn", "--host", "--directory=/src", "--", "make", "-j4"}));
    CHECK(platform::terminalCommand().front() == "sh");
    CHECK(platform::currentSandbox() == Sandbox::None); // the test suite runs unconfined
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-p4-XXXXXX";
    const char* root = g_mkdtemp(tmpl);
    if (!root)
        return 2;
    g_dir = root;
    gtk_init(&argc, &argv);

    std::printf("function list (upstream fixtures)...\n");
    testFunctionListFixtures();
    std::printf("workspaces...\n");
    testWorkspace();
    std::printf("session FileBrowser...\n");
    testSessionFileBrowser();
    std::printf("sandbox...\n");
    testSandbox();

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
