#pragma once
#include <string>
#include <vector>

#include <Processing.NDI.Lib.h>

// Loads the installed NDI runtime dynamically (as the NDI SDK recommends): looks in
// %NDI_RUNTIME_DIR_V6% first, then next to the exe, then the normal DLL search path.
namespace Ndi {
// Idempotent. Returns false (with a message) if the runtime is missing or unusable.
bool Load(std::string* error);
// nullptr until Load() succeeds.
const NDIlib_v6* Api();
// Destroys the shared finder and the library. Call once at exit after all receivers are gone.
void Unload();
// Names of NDI sources currently visible on the network (uses a shared, long-lived finder).
std::vector<std::string> GetSourceNames();
}  // namespace Ndi
