// The overlaid upstream settings registry (src/settings/settings.cpp with root patches 0012 and 0013): a damaged vcs.ini falls back
// to the defaults key by key, the next save repairs it, hidden rows leave no empty page (M08-04, M08-03).
#include "settings/settings.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    int g_failures = 0;

    void check(bool ok, const std::string &what)
    {
        std::printf("%s settings %s\n", ok ? "PASS" : "FAIL", what.c_str());
        if (!ok)
            ++g_failures;
    }

    std::string readAll(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }

    void writeAll(const std::string &path, const std::string &text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
}

int runSettingsTests()
{
    g_failures = 0;
    using settings::Spec;
    using settings::Type;
    int applied = -1;
    {
        Spec s;
        s.key = "test.toggle";
        s.page = "Display";
        s.section = "A";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        settings::add(s);
        s = {};
        s.key = "test.choice";
        s.page = "Display";
        s.section = "A";
        s.type = Type::Choice;
        s.choices = {{"one", 1}, {"two", 2}, {"four", 4}};
        s.defaultValue = 2;
        s.onChange = [&applied](int v) { applied = v; };
        settings::add(s);
        s = {};
        s.key = "test.integer";
        s.page = "Audio";
        s.section = "B";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.defaultValue = 100;
        settings::add(s);
        s = {};
        s.key = "test.key";
        s.page = "Keyboard";
        s.section = "C";
        s.type = Type::Key;
        s.defaultValue = 290;
        settings::add(s);
        s = {};
        s.key = "test.reset";
        s.page = "Advanced";
        s.section = "D";
        s.type = Type::Action;
        settings::add(s);
    }

    const std::string ini = "/tmp/t/vcs.ini";
    // every kind of damage: text, trailing junk, out of range, empty, a line without '=', binary bytes, a NUL
    std::string damaged = "test.toggle=abc\ntest.choice=4x\ntest.integer=250\ntest.key=\ngarbage line\n";
    damaged += std::string("\x01\xff\xfe=\x02\n", 5);
    damaged += std::string("test.choice", 11) + "=" + std::string("1\0", 2) + "\n";
    damaged += "window.x=12\n";
    writeAll(ini, damaged);
    settings::load(ini);
    check(settings::value("test.toggle") == 1, "text 'abc' for a toggle ignored: the default 1, not atoi's 0");
    check(settings::value("test.choice") == 2, "'4x' and '1<NUL>' for a choice ignored: the default 2");
    check(settings::value("test.integer") == 100, "250 above the range ignored: the default 100");
    check(settings::value("test.key") == 290, "an empty key ignored: the default");
    check(settings::state("window.x") == "12", "state lines survive a damaged file");
    settings::applyAll();
    check(applied == 2, "applyAll applies the default after a damaged value");

    writeAll(ini, "test.toggle=0\ntest.choice=4\n test.integer = 35 \n");
    settings::load(ini);
    check(settings::value("test.toggle") == 0 && settings::value("test.choice") == 4 && settings::value("test.integer") == 35,
          "good values load, spaces around them trimmed");
    writeAll(ini, "test.integer=-5\ntest.choice=+4\n");
    settings::load(ini);
    check(settings::value("test.integer") == 35 && settings::value("test.choice") == 4,
          "-5 below the range ignored (the value before stays: load starts from what is set); '+4' taken (strtol)");

    // a damaged file is repaired by the next save: the keys' lines get their real values, other lines are kept
    writeAll(ini, "# player file\ntest.toggle=junk\nwindow.x=12\n");
    settings::load(ini);
    check(settings::set("test.integer", 40), "set a value");
    const std::string saved = readAll(ini);
    const std::string toggleLine = "test.toggle=" + std::to_string(settings::value("test.toggle")) + "\n";
    check(saved.find(toggleLine) != std::string::npos && saved.find("junk") == std::string::npos,
          "the next save writes the damaged key's real value over it");
    check(saved.find("# player file\n") == 0 && saved.find("window.x=12\n") != std::string::npos &&
              saved.find("test.integer=40\n") != std::string::npos,
          "comments and state kept, the change saved");
    settings::load(ini);
    check(settings::value("test.integer") == 40, "relaunch: the saved value loads again");

    // reset to defaults, as the Advanced page's action does
    for (const settings::Spec &each : settings::all())
        if (each.type != Type::Action)
            settings::set(each.key, each.defaultValue);
    settings::load(ini);
    check(settings::value("test.integer") == 100 && settings::value("test.toggle") == 1 && settings::value("test.choice") == 2,
          "reset: every setting back to its default, also after reloading");

    // a missing file: defaults, and the first save creates it
    std::remove(ini.c_str());
    settings::load(ini);
    check(settings::value("test.choice") == 2, "missing file: defaults");
    check(settings::set("test.choice", 1) && readAll(ini).find("test.choice=1\n") != std::string::npos,
          "missing file: the first change creates it");

    // hidden rows: no empty page, unknown keys refused
    auto pages = settings::pages();
    check(pages.size() == 4, "four pages with rows");
    check(settings::setHidden("test.key", true), "hide the keyboard row");
    check(!settings::setHidden("no.such", true), "an unknown key is refused");
    pages = settings::pages();
    bool keyboard = false;
    for (const std::string &p : pages)
        keyboard = keyboard || p == "Keyboard";
    check(pages.size() == 3 && !keyboard, "the page left without a row has no tab");
    check(settings::set("test.key", 291) && settings::value("test.key") == 291, "a hidden setting still takes a value");

    // a host's narrower choices and default (root patch 0013, M09-01: the Switch's display.aspect 4:3 / 16:9, 16:9)
    check(!settings::setChoices("no.such", {{"a", 1}}, 1), "setChoices: an unknown key is refused");
    check(!settings::setChoices("test.toggle", {{"a", 1}}, 1), "setChoices: a setting that is not a Choice is refused");
    check(!settings::setChoices("test.choice", {{"one", 1}, {"four", 4}}, 2), "setChoices: a default outside the choices is refused");
    check(settings::all()[1].choices.size() == 3 && settings::all()[1].defaultValue == 2, "a refused setChoices changes nothing");
    check(settings::setChoices("test.choice", {{"one", 1}, {"four", 4}}, 4, "new help"), "setChoices: narrowed, default 4");
    check(settings::all()[1].help == "new help" && settings::all()[1].choices.size() == 2, "the help and the choices replaced");
    std::remove(ini.c_str());
    settings::load(ini);
    check(settings::value("test.choice") == 4, "no file: the host's default");
    writeAll(ini, "test.choice=2\n");
    settings::load(ini);
    check(settings::value("test.choice") == 4, "a saved value no longer offered (2) ignored: the host's default");
    check(!settings::set("test.choice", 2) && settings::value("test.choice") == 4, "set refuses a value no longer offered");
    writeAll(ini, "test.choice=1\n");
    settings::load(ini);
    check(settings::value("test.choice") == 1, "an offered value loads");
    check(settings::set("test.choice", 4) && readAll(ini).find("test.choice=4\n") != std::string::npos,
          "back to the host's default: the file's line rewritten");
    std::remove(ini.c_str());
    settings::load(ini);
    check(settings::set("test.integer", 41) && readAll(ini).find("test.choice=") == std::string::npos,
          "the host's default is not appended to a new file (only values off the default are)");
    std::printf("%s settings: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures;
}
