# FastDrawingVisual.CommandProtocol

Single-source command protocol generator for native command stream.

## Source of truth

- `command_protocol.schema.json`

## Outputs

- `BridgeCommandProtocol.g.cs`
- `BridgeCommandProtocol.g.h`

## Manual run (optional)

```powershell
dotnet run --project .\FastDrawingVisual.CommandProtocol\FastDrawingVisual.CommandProtocol.csproj -- `
  --schema .\FastDrawingVisual.CommandProtocol\command_protocol.schema.json `
  --out-dir .\artifacts\generated\protocol
```

Consumer projects invoke the generator automatically during build.
