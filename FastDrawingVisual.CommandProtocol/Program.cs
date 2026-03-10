using System.Text;
using System.Text.Json;

internal static class Program
{
    private static int Main()
    {
        var projectDirectory = ResolveProjectDirectory();
        var schemaPath = Path.Combine(projectDirectory, "command_protocol.schema.json");
        var outputDirectory = Path.GetFullPath(Path.Combine(projectDirectory, "..", "artifacts", "generated", "protocol"));

        var schema = LoadSchema(schemaPath);
        ProtocolSchemaValidator.Validate(schema);

        Directory.CreateDirectory(outputDirectory);

        var csPath = Path.Combine(outputDirectory, $"{schema.Name}.g.cs");
        var cppPath = Path.Combine(outputDirectory, $"{schema.Name}.g.h");

        var output = new GeneratedOutput(
            SlotProtocolGenerator.GenerateCSharp(schema),
            SlotProtocolGenerator.GenerateCppHeader(schema));

        File.WriteAllText(csPath, output.CSharp, new UTF8Encoding(false));
        File.WriteAllText(cppPath, output.CppHeader, new UTF8Encoding(false));

        Console.WriteLine("Generated:");
        Console.WriteLine($"  {csPath}");
        Console.WriteLine($"  {cppPath}");
        return 0;
    }

    private static string ResolveProjectDirectory()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var projectFile = Path.Combine(current.FullName, "FastDrawingVisual.CommandProtocol.csproj");
            var schemaFile = Path.Combine(current.FullName, "command_protocol.schema.json");
            if (File.Exists(projectFile) && File.Exists(schemaFile))
                return current.FullName;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not locate FastDrawingVisual.CommandProtocol project directory.");
    }

    private static ProtocolSchema LoadSchema(string schemaPath)
    {
        if (!File.Exists(schemaPath))
            throw new FileNotFoundException($"Schema not found: {schemaPath}");

        var json = File.ReadAllText(schemaPath);
        var schema = JsonSerializer.Deserialize<ProtocolSchema>(json, new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true
        });

        if (schema is null)
            throw new InvalidOperationException($"Failed to deserialize schema: {schemaPath}");

        return schema;
    }
}
