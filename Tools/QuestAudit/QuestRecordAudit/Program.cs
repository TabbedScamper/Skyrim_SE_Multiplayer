using System.Text.Json;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Skyrim;

if (args.Length < 2)
{
    Console.Error.WriteLine("Usage: QuestRecordAudit <plugin-path> <output-json> [editor-id]");
    return 2;
}

var pluginPath = Path.GetFullPath(args[0]);
var outputPath = Path.GetFullPath(args[1]);
var editorIdFilter = args.Length >= 3 ? args[2] : null;

if (!File.Exists(pluginPath))
{
    Console.Error.WriteLine($"Plugin not found: {pluginPath}");
    return 2;
}

var modKey = ModKey.FromFileName(Path.GetFileName(pluginPath));
var modPath = new ModPath(modKey, pluginPath);
using var mod = SkyrimMod.CreateFromBinaryOverlay(modPath, SkyrimRelease.SkyrimSE);

var quests = mod.Quests
    .Where(q => editorIdFilter is null ||
                string.Equals(q.EditorID, editorIdFilter, StringComparison.OrdinalIgnoreCase))
    .OrderBy(q => q.EditorID, StringComparer.OrdinalIgnoreCase)
    .Select(q =>
    {
        var fragments = q.VirtualMachineAdapter?.Fragments
            .OrderBy(f => f.Stage)
            .ThenBy(f => f.StageIndex)
            .Select(f => new
            {
                stage = f.Stage,
                stage_index = f.StageIndex,
                script = f.ScriptName,
                function = f.FragmentName
            })
            .ToArray() ?? [];

        var linkedScenes = mod.Scenes
            .Where(s => !s.Quest.IsNull && s.Quest.FormKey == q.FormKey)
            .OrderBy(s => s.EditorID, StringComparer.OrdinalIgnoreCase)
            .Select(s => new
            {
                form_key = s.FormKey.ToString(),
                editor_id = s.EditorID,
                flags = s.Flags?.ToString(),
                phase_count = s.Phases.Count,
                actor_count = s.Actors.Count,
                action_count = s.Actions.Count,
                condition_count = s.Conditions.Count,
                attached_scripts = s.VirtualMachineAdapter?.Scripts.Select(x => x.Name).ToArray() ?? []
            })
            .ToArray();

        return new
        {
            form_key = q.FormKey.ToString(),
            editor_id = q.EditorID,
            type = q.Type.ToString(),
            flags = q.Flags.ToString(),
            priority = q.Priority,
            filter = q.Filter,
            stages = q.Stages
                .OrderBy(s => s.Index)
                .Select(s => new
                {
                    index = s.Index,
                    flags = s.Flags.ToString(),
                    log_entry_count = s.LogEntries.Count,
                    fragments = fragments.Where(f => f.stage == s.Index).ToArray()
                })
                .ToArray(),
            objectives = q.Objectives
                .OrderBy(o => o.Index)
                .Select(o => new
                {
                    index = o.Index,
                    flags = o.Flags?.ToString(),
                    target_count = o.Targets.Count
                })
                .ToArray(),
            aliases = q.Aliases
                .OrderBy(a => a.ID)
                .Select(a => new
                {
                    id = a.ID,
                    name = a.Name,
                    type = a.Type.ToString(),
                    flags = a.Flags?.ToString(),
                    forced_reference = a.ForcedReference.IsNull ? null : a.ForcedReference.FormKey.ToString(),
                    unique_actor = a.UniqueActor.IsNull ? null : a.UniqueActor.FormKey.ToString(),
                    specific_location = a.SpecificLocation.IsNull ? null : a.SpecificLocation.FormKey.ToString(),
                    condition_count = a.Conditions.Count,
                    package_count = a.PackageData.Count
                })
                .ToArray(),
            quest_scripts = q.VirtualMachineAdapter?.Scripts.Select(x => x.Name).ToArray() ?? [],
            fragment_script = q.VirtualMachineAdapter?.FileName,
            fragments,
            scenes = linkedScenes
        };
    })
    .ToArray();

var result = new
{
    schema_version = 1,
    source_plugin = Path.GetFileName(pluginPath),
    generated_at_utc = DateTime.UtcNow,
    editor_id_filter = editorIdFilter,
    quest_count = quests.Length,
    note = "Derived record metadata only; dialogue and journal text are intentionally omitted.",
    quests
};

Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
await File.WriteAllTextAsync(outputPath, JsonSerializer.Serialize(result, new JsonSerializerOptions
{
    WriteIndented = true
}));

Console.WriteLine($"Wrote {quests.Length} quest record(s) to {outputPath}");
return 0;
