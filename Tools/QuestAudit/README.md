# Quest audit tools

These tools build a reproducible multiplayer-risk inventory from a user's own
installed Skyrim data. They do not patch the game and do not redistribute game
records or scripts.

## Script triage

After privately extracting and decompiling installed PEX files, run:

```powershell
.\Invoke-QuestScriptAudit.ps1 `
  -InputDirectory 'C:\path\to\decompiled\psc' `
  -OutputDirectory 'C:\path\to\audit-output'
```

The report contains only filenames, risk categories, counts, and line numbers.
Scores prioritize human review; they are not proof of defects.

## Plugin record correlation

Build and run the read-only Mutagen inspector:

```powershell
dotnet run --project .\QuestRecordAudit -c Release -- `
  'C:\path\to\Skyrim.esm' `
  'C:\path\to\mq101-record.json' `
  MQ101
```

Omit the final editor ID to export derived metadata for every quest in the
plugin. The JSON omits dialogue and journal text. The project currently targets
.NET 10 because pinned Mutagen 0.54.4 supports .NET 9 and .NET 10.
