#include "logic.h"

#include <cstdio>

static int failures = 0;
#define CHECK(x) ((x) ? void() : (void)(std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x), ++failures))

int main() {
    // KDE wheel/shortcut navigation
    CHECK(stepDesktop(0, 3, -1, false) == 0);
    CHECK(stepDesktop(2, 3, 1, false) == 2);
    CHECK(stepDesktop(1, 3, 1, false) == 2);
    CHECK(stepDesktop(0, 3, -1, true) == 2);
    CHECK(stepDesktop(2, 3, 1, true) == 0);
    CHECK(stepDesktop(0, 0, 1, true) == 0);

    // Showing 3 of 6 desktops: scroll only as far as the current one needs
    CHECK(visibleFirst(0, 2, 3, 6) == 0);
    CHECK(visibleFirst(0, 3, 3, 6) == 1);
    CHECK(visibleFirst(1, 5, 3, 6) == 3);
    CHECK(visibleFirst(3, 2, 3, 6) == 2);
    CHECK(visibleFirst(4, 1, 6, 6) == 0);   // all shown
    CHECK(visibleFirst(5, 4, 3, 5) == 2);   // desktops removed: back in range
    CHECK(visibleFirst(2, -1, 3, 6) == 2);  // current unknown: stay
    // 71 px desktops, 2 px apart, before the taskbar buttons
    CHECK(fittingDesktops(217, 71, 2) == 3 && fittingDesktops(216, 71, 2) == 2);
    CHECK(fittingDesktops(10, 71, 2) == 1 && fittingDesktops(-40, 71, 2) == 1);

    // Outline mapping: maximized window above a 48 px taskbar, 1080p into a 71x40 desktop rect
    const RECT screen{0, 0, 1920, 1080};
    FRect r = mapWindowRect({0, 0, 1920, 1032}, screen, {73, 0, 144, 40});
    CHECK(r.l == 73 && r.t == 0 && r.r == 144 && r.b == 38);
    // parts outside the shown area are clamped away
    r = mapWindowRect({-100, -100, 960, 540}, screen, {0, 0, 192, 108});
    CHECK(r.l == 0 && r.t == 0 && r.r == 96 && r.b == 54);
    // virtual screen with a monitor left of the primary (negative origin)
    r = mapWindowRect({-1920, 0, -960, 1080}, {-1920, 0, 1920, 1080}, {0, 0, 384, 108});
    CHECK(r.l == 0 && r.r == 96 && r.b == 108);

    // Fluent cubic-bezier(0,0,0,1): endpoints, front-loaded, monotonic
    CHECK(fluentDecelerate(0) == 0 && std::abs(fluentDecelerate(1) - 1) < 1e-6f && fluentDecelerate(2) == fluentDecelerate(1));
    CHECK(std::abs(fluentDecelerate(.5f) - .88988f) < 1e-3f && fluentDecelerate(.1f) > .44f);
    for (float t = 0, prev = 0; t <= 1; t += .01f) {
        CHECK(fluentDecelerate(t) >= prev);
        prev = fluentDecelerate(t);
    }

    // Tooltip text, KDE rules
    auto t = tooltipLines({L"A"}, {});
    CHECK(t.size() == 1 && t[0].text == L"A" && !t[0].bullet);
    t = tooltipLines({L"A", L"B", L"C", L"D", L"E"}, {});
    CHECK(t.size() == 6 && t[0].text == L"5 Windows:" && t[5].bullet);  // exactly five: no "…and 1 other"
    t = tooltipLines({L"A", L"B", L"C", L"D", L"E", L"F"}, {L"M"});
    CHECK(t.size() == 9 && t[5].text == L"…and 2 other windows" && t[6].text.empty() && t[7].text == L"1 Minimized Window:" && t[8].bullet);
    t = tooltipLines({}, {L"M", L"N"});
    CHECK(t.size() == 3 && t[0].text == L"2 Minimized Windows:");
    CHECK(tooltipLines({}, {}).empty());

    std::puts(failures ? "selftest FAILED" : "selftest ok");
    return failures != 0;
}
