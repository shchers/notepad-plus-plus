#include "core/Session.h"

#include "platform/FileIO.h"

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>
#include <system_error>

namespace npp::core {

namespace {

const char* const kViewNames[2] = {"mainView", "subView"};

bool yes(const pugi::xml_node& n, const char* attr)
{
    return std::string_view(n.attribute(attr).as_string()) == "yes";
}

const char* yesNo(bool b)
{
    return b ? "yes" : "no";
}

} // namespace

bool readSession(const std::filesystem::path& file, Session& out, std::string* error)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result res = doc.load_file(file.c_str());
    if (!res) {
        if (error)
            *error = file.string() + ": " + res.description();
        return false;
    }
    const pugi::xml_node root = doc.child("NotepadPlus").child("Session");
    if (!root) {
        if (error)
            *error = file.string() + ": not a Notepad++ session file";
        return false;
    }
    out = Session{};
    out.activeView = root.attribute("activeView").as_int(0) == 1 ? 1 : 0;
    for (int k = 0; k < 2; ++k) {
        const pugi::xml_node view = root.child(kViewNames[k]);
        out.activeIndex[k] = static_cast<std::size_t>(std::max(0, view.attribute("activeIndex").as_int(0)));
        for (const pugi::xml_node f : view.children("File")) {
            SessionFile sf;
            sf.filename = f.attribute("filename").as_string();
            if (sf.filename.empty())
                continue;
            sf.lang = f.attribute("lang").as_string();
            sf.encoding = f.attribute("encoding").as_int(-1);
            sf.userReadOnly = yes(f, "userReadOnly");
            sf.backupFilePath = f.attribute("backupFilePath").as_string();
            sf.originalFileLastModifTimestamp =
                (static_cast<std::uint64_t>(f.attribute("originalFileLastModifTimestampHigh").as_uint(0)) << 32) |
                f.attribute("originalFileLastModifTimestamp").as_uint(0);
            sf.firstVisibleLine = f.attribute("firstVisibleLine").as_llong(0);
            sf.xOffset = f.attribute("xOffset").as_llong(0);
            sf.scrollWidth = f.attribute("scrollWidth").as_llong(1);
            sf.startPos = f.attribute("startPos").as_llong(0);
            sf.endPos = f.attribute("endPos").as_llong(0);
            sf.selMode = f.attribute("selMode").as_llong(0);
            sf.offset = f.attribute("offset").as_llong(0);
            sf.wrapCount = f.attribute("wrapCount").as_llong(0);
            sf.tabColourId = f.attribute("tabColourId").as_int(-1);
            sf.rtl = yes(f, "RTL");
            sf.tabPinned = yes(f, "tabPinned");
            sf.untitleTabRenamed = yes(f, "untitleTabRenamed");
            for (const pugi::xml_node m : f.children("Mark"))
                if (const long long l = m.attribute("line").as_llong(-1); l >= 0)
                    sf.marks.push_back(l);
            for (const pugi::xml_node m : f.children("Fold"))
                if (const long long l = m.attribute("line").as_llong(-1); l >= 0)
                    sf.folds.push_back(l);
            out.views[k].push_back(std::move(sf));
        }
    }
    return true;
}

bool writeSession(const std::filesystem::path& file, const Session& s, std::string* error)
{
    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";
    pugi::xml_node session = doc.append_child("NotepadPlus").append_child("Session");
    session.append_attribute("activeView") = s.activeView;
    for (int k = 0; k < 2; ++k) {
        pugi::xml_node view = session.append_child(kViewNames[k]);
        view.append_attribute("activeIndex") = static_cast<unsigned long long>(s.activeIndex[k]);
        for (const SessionFile& sf : s.views[k]) {
            pugi::xml_node f = view.append_child("File");
            f.append_attribute("firstVisibleLine") = static_cast<long long>(sf.firstVisibleLine);
            f.append_attribute("xOffset") = static_cast<long long>(sf.xOffset);
            f.append_attribute("scrollWidth") = static_cast<long long>(sf.scrollWidth);
            f.append_attribute("startPos") = static_cast<long long>(sf.startPos);
            f.append_attribute("endPos") = static_cast<long long>(sf.endPos);
            f.append_attribute("selMode") = static_cast<long long>(sf.selMode);
            f.append_attribute("offset") = static_cast<long long>(sf.offset);
            f.append_attribute("wrapCount") = static_cast<long long>(sf.wrapCount);
            f.append_attribute("lang") = sf.lang.c_str();
            f.append_attribute("encoding") = sf.encoding;
            f.append_attribute("userReadOnly") = yesNo(sf.userReadOnly);
            f.append_attribute("filename") = sf.filename.c_str();
            f.append_attribute("backupFilePath") = sf.backupFilePath.c_str();
            f.append_attribute("originalFileLastModifTimestamp") = static_cast<unsigned>(sf.originalFileLastModifTimestamp & 0xFFFFFFFFu);
            f.append_attribute("originalFileLastModifTimestampHigh") = static_cast<unsigned>(sf.originalFileLastModifTimestamp >> 32);
            f.append_attribute("tabColourId") = sf.tabColourId;
            f.append_attribute("RTL") = yesNo(sf.rtl);
            f.append_attribute("tabPinned") = yesNo(sf.tabPinned);
            if (sf.untitleTabRenamed)
                f.append_attribute("untitleTabRenamed") = "yes";
            for (const std::int64_t l : sf.marks)
                f.append_child("Mark").append_attribute("line") = static_cast<long long>(l);
            for (const std::int64_t l : sf.folds)
                f.append_child("Fold").append_attribute("line") = static_cast<long long>(l);
        }
    }

    std::ostringstream os;
    doc.save(os, "    ", pugi::format_default, pugi::encoding_utf8);
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec))
        platform::copyFile(file, file.string() + ".inCaseOfCorruption.bak", nullptr); // best effort, as upstream
    return platform::writeFileAtomic(file, os.str(), error);
}

} // namespace npp::core
