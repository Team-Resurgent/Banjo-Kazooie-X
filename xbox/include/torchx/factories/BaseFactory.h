/* factories/BaseFactory.h -- Banjo-Kazooie-X: Torch's factory base, as much of it as the BK64 headers Lighthouse
 * reads need (build/upstream/torch/src/factories/bk64/MapFactory.h). Torch's own pulls in yaml-cpp, ImGui, its
 * cartridge reader and binary tools for the extraction side, which runs on a PC (setup.py), not here.
 *
 * The types are Torch's (same names, same members) so its headers compile unchanged; the factories' and exporters'
 * virtual functions are declared, never defined here, and nothing on the Xbox makes one. */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "factories/ResourceType.h"

#define REGISTER(type, c) { ExportType::type, std::make_shared<c>() },

struct OffsetEntry {
    uint32_t start;
    uint32_t end;
};

typedef std::optional<std::variant<size_t, OffsetEntry>> ExportResult;

enum class ExportType { Header, Code, Binary, Modding, XML };

class IParsedData {};

template <typename T> class GenericData : public IParsedData {
  public:
    T mData;
};

class BaseExporter {
  public:
    virtual ExportResult Export(std::ostream& write, std::shared_ptr<IParsedData> data, std::string& entryName,
                                YAML::Node& node, std::string* replacement) = 0;
};

class BaseFactory {
  public:
    virtual std::optional<std::shared_ptr<IParsedData>> parse(std::vector<uint8_t>& buffer, YAML::Node& data) = 0;
    virtual std::optional<std::shared_ptr<IParsedData>> parse_modding(std::vector<uint8_t>& buffer, YAML::Node& data) {
        return std::nullopt;
    }
    virtual bool SupportModdedAssets() {
        return false;
    }
    virtual bool HasModdedDependencies() {
        return false;
    }
    virtual std::unordered_map<ExportType, std::shared_ptr<BaseExporter>> GetExporters() {
        return {};
    }
};
