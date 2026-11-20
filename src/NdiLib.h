#pragma once
#include <string>
#include <vector>

// NDI2SDI_HAS_NDI is set by CMake: 1 when the NDI SDK headers were found, 0 for a
// display-capture-only build (then NDI sources are simply unavailable at run time).
#ifndef NDI2SDI_HAS_NDI
#define NDI2SDI_HAS_NDI 1
#endif

#if NDI2SDI_HAS_NDI
#include <Processing.NDI.Lib.h>
#endif

// Loads the installed NDI runtime dynamically (as the NDI SDK recommends): looks in
// %NDI_RUNTIME_DIR_V6% first, then next to the exe, then the normal DLL search path.
namespace Ndi {
// Idempotent. Returns false (with a message) if the runtime is missing or unusable, or if this
// build has no NDI support.
bool Load(std::string* error);
#if NDI2SDI_HAS_NDI
// nullptr until Load() succeeds.
const NDIlib_v6* Api();
#endif
// Destroys the shared finder and the library. Call once at exit after all receivers are gone.
void Unload();
// Names of NDI sources currently visible on the network (uses a shared, long-lived finder).
std::vector<std::string> GetSourceNames();
}  // namespace Ndi
