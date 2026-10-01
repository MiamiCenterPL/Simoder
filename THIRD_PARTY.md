# Third-party references and dependencies

## Model Context Protocol C# SDK

Optional AI DevTools use `ModelContextProtocol` 2.0.0, the official C# SDK maintained by the Model
Context Protocol project in collaboration with Microsoft. It is licensed under Apache-2.0:
https://github.com/modelcontextprotocol/csharp-sdk

## Microsoft Windows SDK targeting pack

Optional Windows Graphics Capture support compiles against `Microsoft.Windows.SDK.NET.Ref`
10.0.26100.70, published by Microsoft under the package's license terms:
https://www.nuget.org/packages/Microsoft.Windows.SDK.NET.Ref/10.0.26100.70

## OpenSC5

Research reference: [TornadoCookie/OpenSC5](https://github.com/TornadoCookie/OpenSC5),
inspected at commit `0100e3ab15eb7a09fdaa29c8f4616fa86191145f` (MIT).

Relevant reference files:

- [`package.c`](https://github.com/TornadoCookie/OpenSC5/blob/0100e3ab15eb7a09fdaa29c8f4616fa86191145f/src/filetypes/package.c)
- [`prop.c`](https://github.com/TornadoCookie/OpenSC5/blob/0100e3ab15eb7a09fdaa29c8f4616fa86191145f/src/filetypes/prop.c)

SC13 Mod Loader independently validates the structures against local game files. Its RefPack
implementation adds bounds checks and its PROP representation preserves the source buffer;
it does not adopt OpenSC5's unchecked decompressor or lossy parsed-value model.

## MinHook

Runtime API instrumentation uses
[MinHook v1.3.4](https://github.com/TsudaKageyu/minhook/releases/tag/v1.3.4), fetched by CMake
at an exact tag. MinHook uses the 2-clause BSD license. It is not used to guess or install a
game-internal resource hook.

## Historical information

The [SimCityPak discussion](https://community.simtropolis.com/forums/topic/53278-simcitypak-modding-tool/)
is treated as historical context only. Claims from old SimCityPak implementations are not
accepted without validation against the current executable and resources.
