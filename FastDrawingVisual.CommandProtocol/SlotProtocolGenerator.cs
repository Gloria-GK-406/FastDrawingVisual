using Scriban;
using Scriban.Runtime;

internal static class SlotProtocolGenerator
{
    public static string GenerateCSharp(ProtocolSchema schema)
    {
        return RenderTemplate("BridgeCommandProtocol.g.cs.scriban", ProtocolTemplateModel.FromSchema(schema));
    }

    public static string GenerateCppHeader(ProtocolSchema schema)
    {
        return RenderTemplate("BridgeCommandProtocol.g.h.scriban", ProtocolTemplateModel.FromSchema(schema));
    }

    private static string RenderTemplate(string templateFileName, ProtocolTemplateModel model)
    {
        var templatePath = ResolveTemplatePath(templateFileName);
        var templateText = File.ReadAllText(templatePath);
        var template = Template.Parse(templateText, templateFileName);

        if (template.HasErrors)
        {
            var errors = string.Join(Environment.NewLine, template.Messages.Select(message => message.ToString()));
            throw new InvalidOperationException($"Failed to parse template '{templateFileName}':{Environment.NewLine}{errors}");
        }

        var globals = new ScriptObject();
        globals.Import(model, renamer: static member => member.Name);

        var context = new TemplateContext
        {
            MemberRenamer = static member => member.Name,
            StrictVariables = true
        };
        context.PushGlobal(globals);

        var rendered = template.Render(context);
        return NormalizeLineEndings(rendered);
    }

    private static string ResolveTemplatePath(string templateFileName)
    {
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "Templates", templateFileName),
            Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "Templates", templateFileName))
        };

        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
                return candidate;
        }

        throw new FileNotFoundException($"Template not found: {templateFileName}");
    }

    private static string NormalizeLineEndings(string text)
    {
        return text
            .Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace("\r", "\n", StringComparison.Ordinal)
            .Replace("\n", Environment.NewLine, StringComparison.Ordinal);
    }
}
