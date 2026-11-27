#pragma once

// Single source of truth for the mod version shown in logs and the INI header.
// Keep in sync with sider/modules/var_mod.lua (m.version) and CHANGELOG.md.
#ifndef VAR_MOD_VERSION // overridable only by test harnesses
#define VAR_MOD_VERSION "4.5.3"
#endif
