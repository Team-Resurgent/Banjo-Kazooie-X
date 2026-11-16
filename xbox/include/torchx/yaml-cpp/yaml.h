/* yaml-cpp/yaml.h -- Banjo-Kazooie-X: NOT yaml-cpp. Lighthouse reads two of Torch's BK64 headers (the map and the
 * romhack config's data types); those headers name YAML types only in the declarations of Torch's extraction side,
 * which the Xbox never runs (setup.py extracts on a PC). Declared, never defined: a call into yaml-cpp is a link
 * error, never a silent no-op. */
#pragma once

namespace YAML {
class Node;
class Emitter;
} // namespace YAML
