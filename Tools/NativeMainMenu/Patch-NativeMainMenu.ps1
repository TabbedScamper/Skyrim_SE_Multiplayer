[CmdletBinding()]
param(
    [string]$SkyrimPath = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition',
    [string]$ToolCache = (Join-Path $env:LOCALAPPDATA 'SkyrimSEMultiplayer\MainMenuTools')
)

$ErrorActionPreference = 'Stop'

function Assert-Replace {
    param([string]$Text, [string]$Before, [string]$After, [string]$Description)
    if (-not $Text.Contains($Before)) {
        throw "The installed startmenu.swf does not match the expected structure ($Description). No game file was changed."
    }
    return $Text.Replace($Before, $After)
}

$dataPath = Join-Path $SkyrimPath 'Data'
$sourceArchive = Join-Path $dataPath 'Skyrim - Interface.bsa'
$destination = Join-Path $dataPath 'Interface\startmenu.swf'
if (-not (Test-Path -LiteralPath $sourceArchive)) {
    throw "Skyrim interface archive not found: $sourceArchive"
}

$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path -LiteralPath $sevenZip)) {
    throw '7-Zip is required to unpack the open-source BSArch tool. Install 7-Zip and run this script again.'
}

$xeditDir = Join-Path $ToolCache 'xedit-4.1.5f'
$bsarch = Join-Path $xeditDir 'BSArch64.exe'
if (-not (Test-Path -LiteralPath $bsarch)) {
    New-Item -ItemType Directory -Force -Path $xeditDir | Out-Null
    $xeditArchive = Join-Path $xeditDir 'xEdit.4.1.5f.7z'
    Invoke-WebRequest 'https://github.com/TES5Edit/TES5Edit/releases/download/xedit-4.1.5f/xEdit.4.1.5f.7z' -OutFile $xeditArchive
    & $sevenZip e $xeditArchive "-o$xeditDir" BSArch64.exe -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract BSArch64.exe.' }
}

$ffdecDir = Join-Path $ToolCache 'ffdec-26.3.0'
$ffdec = Join-Path $ffdecDir 'ffdec-cli.exe'
if (-not (Test-Path -LiteralPath $ffdec)) {
    New-Item -ItemType Directory -Force -Path $ffdecDir | Out-Null
    $ffdecArchive = Join-Path $ffdecDir 'ffdec_26.3.0.zip'
    Invoke-WebRequest 'https://github.com/jindrapetrik/jpexs-decompiler/releases/download/version26.3.0/ffdec_26.3.0.zip' -OutFile $ffdecArchive
    Expand-Archive -LiteralPath $ffdecArchive -DestinationPath $ffdecDir -Force
}

$work = Join-Path $ToolCache 'work'
$vanilla = Join-Path $work 'vanilla'
$scripts = Join-Path $work 'scripts'
$scriptsRoot = Join-Path $scripts 'scripts'
$output = Join-Path $work 'output\startmenu.swf'
New-Item -ItemType Directory -Force -Path $vanilla,$scripts,(Split-Path $output) | Out-Null

& $bsarch unpack $sourceArchive $vanilla -quiet -mt
if ($LASTEXITCODE -ne 0) { throw 'Could not extract Skyrim - Interface.bsa.' }

$sourceSwf = Join-Path $vanilla 'interface\startmenu.swf'
if (-not (Test-Path -LiteralPath $sourceSwf)) { throw 'The archive did not contain interface\startmenu.swf.' }
$sourceHash = (Get-FileHash -LiteralPath $sourceSwf -Algorithm SHA256).Hash
Write-Host "Vanilla startmenu.swf SHA-256: $sourceHash"

& $ffdec -onerror abort -export script $scripts $sourceSwf | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not decompile startmenu.swf.' }

$startMenuScript = Join-Path $scriptsRoot '__Packages\StartMenu.as'
$text = [IO.File]::ReadAllText($startMenuScript)
$text = Assert-Replace $text `
    '   static var OUNCE_DATA_TRANSFER_INDEX = 12;' `
    "   static var OUNCE_DATA_TRANSFER_INDEX = 12;`r`n   static var SKYRIM_SEAMLESS_COOP_INDEX = 13;`r`n   static var SKYRIM_SEAMLESS_OPTIONS_INDEX = 14;`r`n   var SkyrimSeamlessStageWidth = -1;`r`n   var SkyrimSeamlessStageHeight = -1;`r`n   var SkyrimSeamlessPendingLaunchMode = 0;`r`n   var SkyrimSeamlessConfirmTimer = 0;" `
    'menu index declaration'
$text = Assert-Replace $text `
    '      this.onEnterFrame = null;' `
    '      this.onEnterFrame = Shared.Proxy.create(this,this.SkyrimSeamlessWatchLayout);' `
    'persistent live-layout watcher'
$beforeLayout = @'
   function InitExtensions()
   {
      trace("StartMenu::InitExtensions");
      Shared.GlobalFunc.SetLockFunction();
      this._parent.Lock("BR");
      this.Logo_mc.Lock("BL");
      this.Logo_mc._y -= 80;
      this.GamerTagWidget_mc.Lock("TL");
'@ -replace "`n", "`r`n"
$afterLayout = @'
   function SkyrimSeamlessRefreshLayout()
   {
      this._parent.Lock("BR");
      this.Logo_mc.Lock("BL");
      this.Logo_mc._y -= 80;
      this.GamerTagWidget_mc.Lock("TL");
      this.SkyrimSeamlessStageWidth = Stage.visibleRect.width;
      this.SkyrimSeamlessStageHeight = Stage.visibleRect.height;
   }
   function SkyrimSeamlessWatchLayout()
   {
      if(this.SkyrimSeamlessPendingLaunchMode == 0 && _root.SkyrimSeamlessLaunchMode == 1)
      {
         _root.SkyrimSeamlessLaunchMode = 0;
         this.SkyrimSeamlessPendingLaunchMode = 1;
         this.SkyrimSeamlessSelectNativeEntry(StartMenu.NEW_INDEX);
         gfx.io.GameDelegate.call("NEW",[]);
         gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
         this.SkyrimSeamlessConfirmTimer = setInterval(this,"SkyrimSeamlessConfirmLaunch",100);
      }
      else if(this.SkyrimSeamlessPendingLaunchMode == 0 && _root.SkyrimSeamlessLaunchMode == 2)
      {
         _root.SkyrimSeamlessLaunchMode = 0;
         this.SkyrimSeamlessPendingLaunchMode = 2;
         this.SkyrimSeamlessSelectNativeEntry(StartMenu.CONTINUE_INDEX);
         gfx.io.GameDelegate.call("CONTINUE",[]);
         gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
         this.SkyrimSeamlessConfirmTimer = setInterval(this,"SkyrimSeamlessConfirmLaunch",100);
      }
      if(this.SkyrimSeamlessStageWidth != Stage.visibleRect.width || this.SkyrimSeamlessStageHeight != Stage.visibleRect.height)
      {
         this.SkyrimSeamlessRefreshLayout();
      }
   }
   function SkyrimSeamlessSelectNativeEntry(aEntryIndex)
   {
      var i = 0;
      while(i < this.MainList.entryList.length)
      {
         if(this.MainList.entryList[i].index == aEntryIndex)
         {
            this.MainList.disableSelection = false;
            this.MainList.selectedIndex = i;
            return true;
         }
         i++;
      }
      return false;
   }
   function SkyrimSeamlessConfirmLaunch()
   {
      if(this.SkyrimSeamlessPendingLaunchMode != 0 && this.strCurrentState == StartMenu.MAIN_CONFIRM_STATE && this.ShouldProcessInputs)
      {
         clearInterval(this.SkyrimSeamlessConfirmTimer);
         this.SkyrimSeamlessConfirmTimer = 0;
         this.SkyrimSeamlessPendingLaunchMode = 0;
         this.onAcceptPress();
      }
   }
   function InitExtensions()
   {
      trace("StartMenu::InitExtensions");
      Shared.GlobalFunc.SetLockFunction();
      this.SkyrimSeamlessRefreshLayout();
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text `
    $beforeLayout `
    $afterLayout `
    'idempotent live Stage.visibleRect reflow'
$text = Assert-Replace $text `
    '      this.MainList.entryList.push({text:"$CREDITS",index:StartMenu.CREDITS_INDEX,disabled:false,showIcon:false});' `
    "      this.MainList.entryList.push({text:`"CO-OP`",index:StartMenu.SKYRIM_SEAMLESS_COOP_INDEX,disabled:false,showIcon:false});`r`n      this.MainList.entryList.push({text:`"OPTIONS`",index:StartMenu.SKYRIM_SEAMLESS_OPTIONS_INDEX,disabled:false,showIcon:false});`r`n      this.MainList.entryList.push({text:`"`$CREDITS`",index:StartMenu.CREDITS_INDEX,disabled:false,showIcon:false});" `
    'main menu entry list'
$text = Assert-Replace $text `
    '            default:' `
    "            case StartMenu.SKYRIM_SEAMLESS_COOP_INDEX:`r`n               _root.SkyrimSeamlessCoopRequested = 1;`r`n               gfx.io.GameDelegate.call(`"PlaySound`",[`"UIMenuOK`"]);`r`n               return;`r`n            case StartMenu.SKYRIM_SEAMLESS_OPTIONS_INDEX:`r`n               _root.SkyrimSeamlessOptionsRequested = 1;`r`n               gfx.io.GameDelegate.call(`"SkyrimSeamlessOptions`",[]);`r`n               gfx.io.GameDelegate.call(`"PlaySound`",[`"UIMenuOK`"]);`r`n               return;`r`n            default:" `
    'main menu selection handler'
[IO.File]::WriteAllText($startMenuScript, $text, [Text.UTF8Encoding]::new($false))

& $ffdec -onerror abort -importScript $sourceSwf $output $scriptsRoot
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output)) { throw 'Could not compile the patched startmenu.swf.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $dataPath "SkyrimSEMultiplayerBackups\$stamp-native-main-menu"
New-Item -ItemType Directory -Force -Path $backupDir,(Split-Path $destination) | Out-Null
if (Test-Path -LiteralPath $destination) {
    Copy-Item -LiteralPath $destination -Destination (Join-Path $backupDir 'startmenu.swf')
}
Copy-Item -LiteralPath $output -Destination $destination -Force

$outputHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
Write-Host "Installed native CO-OP and OPTIONS menu: $destination"
Write-Host "Patched SHA-256: $outputHash"
Write-Host "Backup directory: $backupDir"
