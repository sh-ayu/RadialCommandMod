#pragma once

#include <memory>

namespace ll::mod {
class Mod;
}

namespace radial {

void registerRadialCommands(std::weak_ptr<ll::mod::Mod> weakMod);

} // namespace radial
