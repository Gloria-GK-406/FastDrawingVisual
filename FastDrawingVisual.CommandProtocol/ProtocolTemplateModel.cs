internal sealed class ProtocolTemplateModel
{
    public string SchemaName { get; init; } = "";
    public int Version { get; init; }
    public int SlotBytes { get; init; }
    public int CommandHeaderBytes { get; init; }
    public int BlobAlignment { get; init; }
    public string TypeName { get; init; } = "";
    public string LayoutName { get; init; } = "";
    public string ColorName { get; init; } = "";
    public string BlobRefName { get; init; } = "";
    public string PacketName { get; init; } = "";
    public string WriterName { get; init; } = "";
    public string FirstCommandName { get; init; } = "";
    public List<CommandTemplateModel> Commands { get; init; } = new();

    public static ProtocolTemplateModel FromSchema(ProtocolSchema schema)
    {
        var orderedCommands = schema.Commands.OrderBy(c => c.Id).ToList();
        var prefix = GeneratorText.GetProtocolPrefix(schema.Name);
        var colorName = $"{prefix}ColorArgb8";
        var layoutName = $"{prefix}Layout";
        var commandHeaderBytes = ProtocolTypeLayout.GetCommandHeaderBytes(schema);

        var commands = orderedCommands
            .Select(command => CommandTemplateModel.FromSchema(command, commandHeaderBytes, colorName))
            .ToList();

        return new ProtocolTemplateModel
        {
            SchemaName = schema.Name,
            Version = Math.Max(schema.Version, 2),
            SlotBytes = schema.SlotBytes!.Value,
            CommandHeaderBytes = commandHeaderBytes,
            BlobAlignment = ProtocolTypeLayout.GetBlobAlignment(schema),
            TypeName = $"{prefix}Type",
            LayoutName = layoutName,
            ColorName = colorName,
            BlobRefName = $"{prefix}BlobRef",
            PacketName = $"{prefix}Packet",
            WriterName = $"{prefix}BufferWriter",
            FirstCommandName = orderedCommands[0].Name,
            Commands = commands
        };
    }
}

internal sealed class CommandTemplateModel
{
    public string Name { get; init; } = "";
    public int Id { get; init; }
    public int SlotCount { get; init; }
    public int PayloadBytes { get; init; }
    public string CSharpParameterList { get; init; } = "";
    public List<FieldTemplateModel> Fields { get; init; } = new();
    public List<FieldTemplateModel> BlobFields { get; init; } = new();

    public static CommandTemplateModel FromSchema(
        CommandSchema command,
        int commandHeaderBytes,
        string colorName)
    {
        var offset = commandHeaderBytes;
        var fields = new List<FieldTemplateModel>(command.Fields.Count);
        foreach (var field in command.Fields)
        {
            var fieldModel = FieldTemplateModel.FromSchema(command.Name, field, offset, colorName);
            fields.Add(fieldModel);
            offset += ProtocolTypeLayout.GetSlotTypeSize(field.Type);
        }

        return new CommandTemplateModel
        {
            Name = command.Name,
            Id = command.Id,
            SlotCount = command.Slots!.Value,
            PayloadBytes = ProtocolTypeLayout.GetPayloadBytes(command),
            CSharpParameterList = string.Join(", ", fields.Select(field => $"{field.CSharpParameterType} {field.CamelName}")),
            Fields = fields,
            BlobFields = fields.Where(field => field.IsBlobRef).ToList()
        };
    }
}

internal sealed class FieldTemplateModel
{
    public string Name { get; init; } = "";
    public string CamelName { get; init; } = "";
    public string CppName { get; init; } = "";
    public string CSharpParameterType { get; init; } = "";
    public string CppFieldType { get; init; } = "";
    public int Offset { get; init; }
    public bool IsBlobRef { get; init; }
    public string CSharpWriteMethod { get; init; } = "";
    public string CSharpValueExpression { get; init; } = "";
    public string CppReadExpression { get; init; } = "";

    public static FieldTemplateModel FromSchema(
        string commandName,
        FieldSchema field,
        int offset,
        string colorName)
    {
        var camelName = GeneratorText.ToCamelCase(field.Name);
        return new FieldTemplateModel
        {
            Name = field.Name,
            CamelName = camelName,
            CppName = GeneratorText.ToCppFieldName(field.Name),
            CSharpParameterType = GetCSharpParameterType(colorName, field.Type),
            CppFieldType = GetCppFieldType(field.Type),
            Offset = offset,
            IsBlobRef = string.Equals(field.Type, "blob_ref", StringComparison.Ordinal),
            CSharpWriteMethod = GetCSharpWriteMethod(field.Type),
            CSharpValueExpression = string.Equals(field.Type, "blob_ref", StringComparison.Ordinal)
                ? $"{camelName}Ref"
                : camelName,
            CppReadExpression = GetCppReadExpression(commandName, field)
        };
    }

    private static string GetCSharpParameterType(string colorName, string type)
    {
        return type switch
        {
            "f32" => "float",
            "u32" => "uint",
            "color_argb8" => colorName,
            "blob_ref" => "ReadOnlySpan<byte>",
            _ => throw new InvalidOperationException($"Unsupported slot field type: {type}")
        };
    }

    private static string GetCppFieldType(string type)
    {
        return type switch
        {
            "f32" => "float",
            "u32" => "std::uint32_t",
            "color_argb8" => "ColorArgb8",
            "blob_ref" => "BlobRef",
            _ => throw new InvalidOperationException($"Unsupported slot field type: {type}")
        };
    }

    private static string GetCSharpWriteMethod(string type)
    {
        return type switch
        {
            "f32" => "WriteSingle",
            "u32" => "WriteUInt32",
            "color_argb8" => "WriteColor",
            "blob_ref" => "WriteBlobRef",
            _ => throw new InvalidOperationException($"Unsupported slot field type: {type}")
        };
    }

    private static string GetCppReadExpression(string commandName, FieldSchema field)
    {
        var offsetConstant = $"k{commandName}{field.Name}Offset";
        return field.Type switch
        {
            "f32" => $"ReadF32(command + {offsetConstant})",
            "u32" => $"ReadU32(command + {offsetConstant})",
            "color_argb8" => $"ReadColorArgb8(command + {offsetConstant})",
            "blob_ref" => $"ReadBlobRef(command + {offsetConstant})",
            _ => throw new InvalidOperationException($"Unsupported slot field type: {field.Type}")
        };
    }
}
