#pragma once
#include "assetfiles.h"
#include <memory>
namespace assetfiles
{
    // "material:<name>" ids referenced by an .xmodel_bin (LZ4) or .xmodel_export
    std::set<std::string> ModelMaterials(const std::string& modelBytes);
    class Resolver
    {
        struct Impl;
        std::unique_ptr<Impl> impl_;
    public:
        explicit Resolver(const fs::path& root);
        ~Resolver();
        bool Find(const std::string& id, Asset& result);
        bool Exists(const std::string& id); // installed definition, even without export sources
        std::set<std::string> References(const std::string& mapText);
        std::vector<std::string> Bytes(const Asset& asset);
    };
}
