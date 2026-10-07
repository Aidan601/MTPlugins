#pragma once
#include <string>
#include <vector>

// A .map file as ordered text blocks keyed by GUID (an entity's key/value lines, each brush a block), the unit Team Create exchanges. Serialize() reproduces a file Radiant loads.
//   entity header:  "{\nguid \"{...}\"\n\"classname\" \"x\"\n\"key\" \"value\"\n..."   (no closing brace)
//   brush block:    " {\n  guid \"{...}\"\n  mesh\n  {\n ...\n  }\n }\n"                (the "// brush N" line is not stored)
namespace mapmodel
{
    struct Brush { std::string guid; std::string text; };
    struct Entity
    {
        std::string        guid;
        std::string        header;        // "{" + key/value lines, without brushes and without the closing "}"
        std::vector<Brush> brushes;
        std::string        classname;     // convenience, parsed from the header
    };
    struct Model
    {
        std::string         fileHeader;   // everything before "// entity 0" (iwmap line, script_startingnumber, layers)
        std::vector<Entity> entities;

        Entity*       FindEntity(const std::string& guid);
        const Entity* FindEntity(const std::string& guid) const;
        Brush*        FindBrush(const std::string& guid, Entity** owner = nullptr);
        bool          RemoveBrush(const std::string& guid);
        bool          RemoveEntity(const std::string& guid);
    };

    // Parse a whole .map file (or Radiant's entity writer output wrapped in a file)
    bool ParseMap(const std::string& text, Model& out, std::string* error = nullptr);
    // Parse ONE entity as written by Entity_Write ("{ ... }" with its brushes)
    bool ParseEntity(const std::string& text, Entity& out, std::string* error = nullptr);
    std::string Serialize(const Model& m);
    std::string SerializeEntity(const Entity& e, int* brushCounter = nullptr);
    std::string ExtractGuid(const std::string& blockText);   // first guid in a block, or ""
    std::string ClassnameOfHeader(const std::string& header);
    std::string ReplaceGuid(const std::string& blockText, const std::string& newGuid);   // rewrites the first guid line
}
