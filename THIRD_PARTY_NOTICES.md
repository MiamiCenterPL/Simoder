# Third-party components and provenance

Simoder's original code is licensed under the root MIT LICENSE. That license
does not replace the licenses or copyright notices of external components.

| Component | Use | License / source |
| --- | --- | --- |
| MinHook v1.3.4, including Hacker Disassembler Engine | Downloaded by CMake and linked into the loader | BSD-2-Clause; retain the complete upstream LICENSE.txt, including HDE notices. https://github.com/TsudaKageyu/minhook |
| Dear ImGui v1.92.9 and Win32/DX9 backends | Downloaded by CMake and linked into the overlay | MIT, copyright Omar Cornut. https://github.com/ocornut/imgui |
| ModelContextProtocol 2.0.0 and ModelContextProtocol.Core | NuGet dependencies of optional AI DevTools | Apache-2.0, as declared by the installed package metadata. https://github.com/modelcontextprotocol/csharp-sdk |
| Microsoft.Extensions.Hosting 10.0.10 and its dependencies | NuGet dependency of optional AI DevTools | Hosting declares MIT; transitive packages retain their individual terms. https://github.com/dotnet/dotnet |
| .NET / Windows SDK runtime components | Optional self-contained AI DevTools and Windows API projection | Retain the applicable Microsoft runtime and package notices; these components are not relicensed by Simoder. |

The native installer copies the full MinHook and Dear ImGui licenses into
`simoder/licenses`. Self-contained .NET distribution contains additional runtime
and transitive package components: this table identifies direct dependencies,
and is not an exhaustive binary-distribution license inventory.

The OpenSC5 and OpenSCP integrations are independently implemented importers
for external catalogs and JSON exports. Their source trees and catalogs are
not bundled as tracked project sources. Imported names, descriptions, comments,
and other data retain their source's terms; provenance hashes do not grant
redistribution rights. Users must check those terms before sharing imported data.

TOON parsing is a local implementation based on the public format specification,
not a bundled third-party parser dependency.

EA game binaries, package files, and game assets are excluded from the repository
and are not covered by Simoder's MIT license. Tests use synthetic fixtures.
