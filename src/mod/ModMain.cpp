#include "mod/RadialMod.h"

#include <ll/api/mod/RegisterHelper.h>

namespace radial {

struct ModEntry {
    bool load() { return RadialMod::instance().load(); }
    bool enable() { return RadialMod::instance().enable(); }
    bool disable() { return RadialMod::instance().disable(); }
};

ModEntry entry;

} // namespace radial

LL_REGISTER_MOD(radial::ModEntry, radial::entry)
