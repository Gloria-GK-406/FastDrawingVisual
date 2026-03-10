# FastDrawingVisual.CommandProtocol

Single-source command protocol generator for native command stream.

## Source of truth

- `command_protocol.schema.json`

## Outputs

- `BridgeCommandProtocol.g.cs`
- `BridgeCommandProtocol.g.h`

Generated outputs include:
- fixed-slot command layout constants
- native payload structs and `CommandReader` on the C++ side

Managed runtime concerns such as packet ownership, buffer growth, and command writing
live in `FastDrawingVisual.CommandRuntime`. The generator now renders the protocol
outputs from Scriban templates under `Templates/`.

## Manual run (optional)

```powershell
dotnet run --project .\FastDrawingVisual.CommandProtocol\FastDrawingVisual.CommandProtocol.csproj
```

The generator always reads `FastDrawingVisual.CommandProtocol/command_protocol.schema.json`
and writes to `artifacts/generated/protocol`.

Consumer projects invoke the generator automatically during build.
