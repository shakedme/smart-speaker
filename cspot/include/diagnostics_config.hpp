#pragma once

#ifndef CONFIG_DIAGNOSTICS_ENABLED
#define CONFIG_DIAGNOSTICS_ENABLED 1
#endif

namespace btspeaker {

// Project-wide runtime toggle for periodic and tagged diagnostic logs.
// Set to false to silence [*_DIAG] output without rebuilding.
inline bool diagnostics_enabled = (CONFIG_DIAGNOSTICS_ENABLED != 0);

}  // namespace btspeaker
