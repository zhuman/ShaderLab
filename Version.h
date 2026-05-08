#pragma once

// ShaderLab application version.
// Increment on each release:
//   MAJOR: breaking changes (graph format incompatible)
//   MINOR: new features (new node types, effects, properties)
//   PATCH: bug fixes (graph format unchanged)
//
// Graph format version tracks the serialization format independently.
// It only increments when the JSON schema changes in a way that older
// versions cannot safely load (new required fields, structural changes).

namespace ShaderLab
{
    constexpr uint32_t VersionMajor = 1;
    constexpr uint32_t VersionMinor = 6;
    constexpr uint32_t VersionPatch = 3;

    // Human-readable version string.
    constexpr const wchar_t* VersionString = L"1.6.3";

    // Graph format version. Increment when serialization format changes.
    // Graphs saved with a higher format version cannot be loaded by older apps.
    constexpr uint32_t GraphFormatVersion = 2;
}
