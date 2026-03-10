using System.Text;

internal sealed record GeneratedOutput(string CSharp, string CppHeader);

internal sealed class ProtocolSchema
{
    public string Name { get; init; } = "";
    public string Endianness { get; init; } = "little";
    public string ColorLayout { get; init; } = "A,R,G,B";
    public int? SlotBytes { get; init; }
    public int? CommandHeaderBytes { get; init; }
    public int? BlobAlignment { get; init; }
    public int Version { get; init; } = 1;
    public List<CommandSchema> Commands { get; init; } = new();
}

internal sealed class CommandSchema
{
    public string Name { get; init; } = "";
    public int Id { get; init; }
    public int? Slots { get; init; }
    public List<FieldSchema> Fields { get; init; } = new();
}

internal sealed class FieldSchema
{
    public string Name { get; init; } = "";
    public string Type { get; init; } = "";
}

internal static class ProtocolSchemaValidator
{
    public static void Validate(ProtocolSchema schema)
    {
        if (string.IsNullOrWhiteSpace(schema.Name))
            throw new InvalidOperationException("Schema.name is required.");

        if (!string.Equals(schema.Endianness, "little", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Only little endianness is supported.");

        if (!string.Equals(schema.ColorLayout, "A,R,G,B", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Only A,R,G,B color layout is supported.");

        if (schema.Commands.Count == 0)
            throw new InvalidOperationException("Schema.commands must not be empty.");

        ValidateSlotProtocol(schema);
    }

    private static void ValidateSlotProtocol(ProtocolSchema schema)
    {
        var slotBytes = schema.SlotBytes.GetValueOrDefault();
        var headerBytes = ProtocolTypeLayout.GetCommandHeaderBytes(schema);
        var blobAlignment = ProtocolTypeLayout.GetBlobAlignment(schema);

        if (slotBytes <= 0 || (slotBytes % 8) != 0)
            throw new InvalidOperationException("Schema.slotBytes must be a positive multiple of 8.");
        if (headerBytes <= 0 || (headerBytes % 8) != 0)
            throw new InvalidOperationException("Schema.commandHeaderBytes must be a positive multiple of 8.");
        if (headerBytes > slotBytes)
            throw new InvalidOperationException("Schema.commandHeaderBytes must not exceed slotBytes.");
        if (blobAlignment <= 0 || (blobAlignment % 8) != 0)
            throw new InvalidOperationException("Schema.blobAlignment must be a positive multiple of 8.");

        var seenNames = new HashSet<string>(StringComparer.Ordinal);
        var seenIds = new HashSet<int>();
        foreach (var cmd in schema.Commands)
        {
            ValidateCommandIdentity(cmd, seenNames, seenIds, ushort.MaxValue);

            if (!cmd.Slots.HasValue || cmd.Slots.Value <= 0)
                throw new InvalidOperationException($"Slot command {cmd.Name} must declare slots > 0.");

            var payloadCapacity = checked(cmd.Slots.Value * slotBytes) - headerBytes;
            if (payloadCapacity < 0)
                throw new InvalidOperationException($"Command {cmd.Name} payload capacity is negative.");

            var fieldNames = new HashSet<string>(StringComparer.Ordinal);
            var payloadBytes = 0;
            foreach (var field in cmd.Fields)
            {
                ValidateField(cmd.Name, field, fieldNames);
                payloadBytes += ProtocolTypeLayout.GetSlotTypeSize(field.Type);
            }

            if (payloadBytes > payloadCapacity)
            {
                throw new InvalidOperationException(
                    $"Command {cmd.Name} payload bytes {payloadBytes} exceed slot capacity {payloadCapacity}.");
            }
        }
    }

    private static void ValidateCommandIdentity(
        CommandSchema cmd,
        HashSet<string> seenNames,
        HashSet<int> seenIds,
        int maxId)
    {
        if (string.IsNullOrWhiteSpace(cmd.Name))
            throw new InvalidOperationException("Command.name is required.");
        if (!seenNames.Add(cmd.Name))
            throw new InvalidOperationException($"Duplicate command name: {cmd.Name}");
        if (!seenIds.Add(cmd.Id))
            throw new InvalidOperationException($"Duplicate command id: {cmd.Id}");
        if (cmd.Id <= 0 || cmd.Id > maxId)
            throw new InvalidOperationException($"Command id must be 1..{maxId}: {cmd.Name}={cmd.Id}");
        if (cmd.Fields.Count == 0)
            throw new InvalidOperationException($"Command {cmd.Name} must contain at least one field.");
    }

    private static void ValidateField(string commandName, FieldSchema field, HashSet<string> fieldNames)
    {
        if (string.IsNullOrWhiteSpace(field.Name))
            throw new InvalidOperationException($"Command {commandName} has empty field name.");
        if (!fieldNames.Add(field.Name))
            throw new InvalidOperationException($"Command {commandName} has duplicate field name: {field.Name}");
        if (string.IsNullOrWhiteSpace(field.Type))
            throw new InvalidOperationException($"Command {commandName}.{field.Name} has empty type.");
    }
}

internal static class ProtocolTypeLayout
{
    public static int GetCommandHeaderBytes(ProtocolSchema schema) => schema.CommandHeaderBytes.GetValueOrDefault(8);

    public static int GetBlobAlignment(ProtocolSchema schema) => schema.BlobAlignment.GetValueOrDefault(8);

    public static int GetSlotTypeSize(string type)
    {
        return type switch
        {
            "f32" => 4,
            "u32" => 4,
            "color_argb8" => 4,
            "blob_ref" => 8,
            _ => throw new InvalidOperationException($"Unsupported slot field type: {type}")
        };
    }

    public static int GetPayloadBytes(CommandSchema command)
    {
        var total = 0;
        foreach (var field in command.Fields)
        {
            total += GetSlotTypeSize(field.Type);
        }

        return total;
    }
}

internal static class GeneratorText
{
    public static string GetProtocolPrefix(string schemaName)
    {
        const string suffix = "Protocol";
        return schemaName.EndsWith(suffix, StringComparison.Ordinal)
            ? schemaName[..^suffix.Length]
            : schemaName;
    }

    public static string ToCamelCase(string text)
    {
        if (string.IsNullOrEmpty(text))
            return text;
        if (text.Length == 1)
            return text.ToLowerInvariant();
        return char.ToLowerInvariant(text[0]) + text[1..];
    }

    public static string ToCppFieldName(string text) => ToCamelCase(text);
}
