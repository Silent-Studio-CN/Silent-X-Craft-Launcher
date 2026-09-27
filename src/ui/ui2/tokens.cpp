/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "tokens.h"

namespace sxcl::ui2 {
namespace {
const Tokens kDark{"#18181c", "#1e1e23", "#24242a", "#2e2e36", "#f0f0f5",
                   "#a8a8b2", "#299cff", "#36363e"};
const Tokens kLight{"#f6f6f8", "#eeeef1", "#ffffff", "#f0f0f4", "#16161a",
                    "#5c5c66", "#0f6cbd", "#e2e2e8"};
bool g_dark = true;
} // namespace

const Tokens &tokens() { return g_dark ? kDark : kLight; }
bool darkTheme() { return g_dark; }
void setDarkTheme(bool dark) { g_dark = dark; }
QColor tokenColor(const char *hex) { return QColor(QString::fromLatin1(hex)); }

} // namespace sxcl::ui2
