[CmdletBinding()]
param(
    [string]$SkyrimPath = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition',
    [string]$ToolCache = (Join-Path $env:LOCALAPPDATA 'SkyrimSEMultiplayer\MainMenuTools'),
    [string]$WorkPath,
    [switch]$PreviewOnly
)

$ErrorActionPreference = 'Stop'

function Assert-Replace {
    param([string]$Text, [string]$Before, [string]$After, [string]$Description)
    $first = $Text.IndexOf($Before, [StringComparison]::Ordinal)
    if ($first -lt 0 -or $Text.IndexOf($Before, $first + $Before.Length, [StringComparison]::Ordinal) -ge 0) {
        throw "The installed quest_journal.swf does not match the expected structure ($Description). No game file was changed."
    }
    return $Text.Substring(0, $first) + $After + $Text.Substring($first + $Before.Length)
}

function Patch {
    param([string]$Before, [string]$After, [string]$Description)
    $script:text = Assert-Replace $script:text (($Before -replace "`r`n", "`n") -replace "`n", "`r`n") (($After -replace "`r`n", "`n") -replace "`n", "`r`n") $Description
}

$dataPath = Join-Path $SkyrimPath 'Data'
$sourceArchive = Join-Path $dataPath 'Skyrim - Interface.bsa'
$destination = Join-Path $dataPath 'Interface\quest_journal.swf'
if (-not (Test-Path -LiteralPath $sourceArchive)) { throw "Skyrim interface archive not found: $sourceArchive" }
$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path -LiteralPath $sevenZip)) { throw '7-Zip is required to unpack BSArch.' }
$xeditDir = Join-Path $ToolCache 'xedit-4.1.5f'
$bsarch = Join-Path $xeditDir 'BSArch64.exe'
if (-not (Test-Path -LiteralPath $bsarch)) {
    New-Item -ItemType Directory -Force -Path $xeditDir | Out-Null
    $archive = Join-Path $xeditDir 'xEdit.4.1.5f.7z'
    Invoke-WebRequest 'https://github.com/TES5Edit/TES5Edit/releases/download/xedit-4.1.5f/xEdit.4.1.5f.7z' -OutFile $archive
    & $sevenZip e $archive "-o$xeditDir" BSArch64.exe -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract BSArch64.exe.' }
}
$ffdecDir = Join-Path $ToolCache 'ffdec-26.3.0'
$ffdec = Join-Path $ffdecDir 'ffdec-cli.exe'
if (-not (Test-Path -LiteralPath $ffdec)) {
    New-Item -ItemType Directory -Force -Path $ffdecDir | Out-Null
    $archive = Join-Path $ffdecDir 'ffdec_26.3.0.zip'
    Invoke-WebRequest 'https://github.com/jindrapetrik/jpexs-decompiler/releases/download/version26.3.0/ffdec_26.3.0.zip' -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $ffdecDir -Force
}
$work = if ($WorkPath) { $WorkPath } else { Join-Path $ToolCache 'journal-work' }
$vanilla = Join-Path $work 'vanilla'
$scripts = Join-Path $work 'scripts'
$scriptsRoot = Join-Path $scripts 'scripts'
$output = Join-Path $work 'output\quest_journal.swf'
New-Item -ItemType Directory -Force -Path $vanilla,$scripts,(Split-Path $output) | Out-Null
& $bsarch unpack $sourceArchive $vanilla -quiet -mt
if ($LASTEXITCODE -ne 0) { throw 'Could not extract Skyrim - Interface.bsa.' }
$sourceSwf = Join-Path $vanilla 'interface\quest_journal.swf'
if (-not (Test-Path -LiteralPath $sourceSwf)) { throw 'The archive did not contain interface\quest_journal.swf.' }
Write-Host "Vanilla quest_journal.swf SHA-256: $((Get-FileHash -LiteralPath $sourceSwf -Algorithm SHA256).Hash)"
& $ffdec -onerror abort -export script $scripts $sourceSwf | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not decompile quest_journal.swf.' }
$systemPage = Join-Path $scriptsRoot '__Packages\SystemPage.as'
$script:text = [IO.File]::ReadAllText($systemPage)

Patch '   static var CONTROLS_CATEGORY_STATE = 18;' @'
   static var CONTROLS_CATEGORY_STATE = 18;
   static var SSM_COOP_STATE = 19;
   var SkyrimSeamlessRoleStamp = -1;
   var SkyrimSeamlessMemberMode = "";
   var SkyrimSeamlessPendingConfirm = "";
   var SkyrimSeamlessSystemEntries;
   var SkyrimSeamlessSystemIndex = 0;
   var SkyrimSeamlessSystemScroll = 0;
'@ 'state declaration'
Patch @'
      this.CategoryList.entryList.push({text:"$QUIT"});
      this.CategoryList.InvalidateData();
'@ @'
      this.CategoryList.entryList.push({text:"$QUIT"});
      // Appended after QUIT so Bethesda's positional SetSaveDisabled arguments stay valid.
      this.CategoryList.entryList.push({text:"CO-OP"});
      this.CategoryList.entryList.push({text:"OPTIONS"});
      this.CategoryList.InvalidateData();
      this.onEnterFrame = Shared.Proxy.create(this,this.SkyrimSeamlessTick);
'@ 'system entries'
Patch '   function SetShowMod(show)' @'
   function SkyrimSeamlessTick()
   {
      if(this.bMenuClosing && this.SkyrimSeamlessSystemEntries != undefined)
         this.SkyrimSeamlessRestoreSystem();
      if(this.iCurrentState == SystemPage.SSM_COOP_STATE && this.SkyrimSeamlessRoleStamp != Number(_root.SSMJournalStamp))
      {
         this.SkyrimSeamlessRoleStamp = Number(_root.SSMJournalStamp);
         if(this.SkyrimSeamlessMemberMode != "")
            this.SkyrimSeamlessShowMembers(this.SkyrimSeamlessMemberMode);
         else
            this.SkyrimSeamlessShowCoop();
      }
      if(Number(_root.SSMJournalBridgeAction) > 0)
      {
         var action = Number(_root.SSMJournalBridgeAction);
         _root.SSMJournalBridgeAction = 0;
         if(action >= 12 && this.iCurrentState != SystemPage.SSM_COOP_STATE)
            return;
         if((action == 10 || action == 11) && this.iCurrentState != SystemPage.MAIN_STATE)
            return;
         if(action == 10)
            this.SkyrimSeamlessShowCoop();
         else if(action == 11)
            this.SkyrimSeamlessRequest(2);
         else if(action == 12)
            this.SkyrimSeamlessChoose("leave");
         else if(action == 13)
            this.SkyrimSeamlessChoose("end");
         else if(action == 14)
            this.SkyrimSeamlessChoose("host");
         else if(action == 15)
            this.SkyrimSeamlessChoose("invite");
         else if(action == 16)
            this.SkyrimSeamlessChoose("access");
         else if(action == 17)
            this.SkyrimSeamlessChoose("promote");
         else if(action == 18)
            this.SkyrimSeamlessChoose("remove");
         else if(action == 19)
            this.SkyrimSeamlessChoose("party");
      }
   }
   function SkyrimSeamlessRequest(action, target)
   {
      _root.SSMJournalTarget = target == undefined ? 0 : target;
      _root.SSMJournalAction = action;
      gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
   }
   function SkyrimSeamlessShowCoop()
   {
      if(this.iCurrentState != SystemPage.MAIN_STATE && this.iCurrentState != SystemPage.SSM_COOP_STATE)
         return;
      if(this.SkyrimSeamlessSystemEntries == undefined)
      {
         this.SkyrimSeamlessSystemEntries = this.CategoryList.entryList;
         this.SkyrimSeamlessSystemIndex = this.CategoryList.selectedIndex;
         this.SkyrimSeamlessSystemScroll = this.CategoryList.scrollPosition;
      }
      this.SkyrimSeamlessMemberMode = "";
      var role = Number(_root.SSMJournalRole);
      if(isNaN(role)) role = 0;
      var entries = [];
      if(role == 0)
      {
         entries.push({text:"HOST",ssmAction:"host"});
         entries.push({text:"INVITE FRIEND",ssmAction:"invite"});
      }
      else
      {
         entries.push({text:"PARTY",ssmAction:"party"});
         entries.push({text:"INVITE FRIEND",ssmAction:"invite"});
         if(role == 1)
         {
            entries.push({text:_root.SSMJournalOpen == 1 ? "CLOSE SESSION" : "OPEN SESSION",ssmAction:"access"});
            entries.push({text:"MAKE HOST",ssmAction:"promote"});
            entries.push({text:"REMOVE PLAYER",ssmAction:"remove"});
            entries.push({text:"END PARTY",ssmAction:"end"});
         }
         else
            entries.push({text:"LEAVE PARTY",ssmAction:"leave"});
      }
      entries.push({text:"BACK",ssmAction:"back"});
      this.CategoryList.selectedIndex = -1;
      this.CategoryList.entryList = entries;
      this.CategoryList.scrollPosition = 0;
      this.CategoryList.InvalidateData();
      this.CategoryList.selectedIndex = 0;
      this.currentState = SystemPage.SSM_COOP_STATE;
   }
   function SkyrimSeamlessRestoreSystem()
   {
      if(this.SkyrimSeamlessSystemEntries == undefined)
         return;
      this.CategoryList.selectedIndex = -1;
      this.CategoryList.entryList = this.SkyrimSeamlessSystemEntries;
      this.SkyrimSeamlessSystemEntries = undefined;
      this.SkyrimSeamlessMemberMode = "";
      this.SkyrimSeamlessPendingConfirm = "";
      this.CategoryList.InvalidateData();
      this.CategoryList.scrollPosition = this.SkyrimSeamlessSystemScroll;
      this.CategoryList.selectedIndex = this.SkyrimSeamlessSystemIndex;
      if(this.iCurrentState == SystemPage.SSM_COOP_STATE)
         this.currentState = SystemPage.MAIN_STATE;
      if(!this.bMenuClosing)
         this.UpdatePermissions();
   }
   function SkyrimSeamlessShowMembers(mode)
   {
      if(Number(_root.SSMJournalRole) != 1)
      {
         this.SkyrimSeamlessShowCoop();
         return;
      }
      var entries = [];
      var i = 0;
      while(i < Number(_root.SSMJournalCount) && i < 16)
      {
         var id = Number(_root["SSMJournalMember" + i]);
         if(id > 0 && id != Number(_root.SSMJournalSelf))
            entries.push({text:String(_root["SSMJournalName" + i]),ssmAction:"member",ssmId:id});
         i++;
      }
      if(entries.length == 0)
      {
         this.SkyrimSeamlessShowCoop();
         gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
         return;
      }
      this.SkyrimSeamlessMemberMode = mode;
      entries.push({text:"BACK",ssmAction:"back"});
      this.CategoryList.selectedIndex = -1;
      this.CategoryList.entryList = entries;
      this.CategoryList.scrollPosition = 0;
      this.CategoryList.InvalidateData();
      this.CategoryList.selectedIndex = 0;
   }
   function SkyrimSeamlessChoose(action, target)
   {
      var role = Number(_root.SSMJournalRole);
      if(action == "back")
      {
         if(this.SkyrimSeamlessMemberMode != "") this.SkyrimSeamlessShowCoop();
         else this.SkyrimSeamlessRestoreSystem();
      }
      else if(action == "party" && role != 0) this.SkyrimSeamlessRequest(1);
      else if(action == "invite") this.SkyrimSeamlessRequest(4);
      else if(action == "host" && role == 0) this.SkyrimSeamlessRequest(3);
      else if(action == "access" && role == 1) this.SkyrimSeamlessRequest(5);
      else if(action == "promote" || action == "remove") this.SkyrimSeamlessShowMembers(action);
      else if(action == "member" && role == 1)
      {
         this.SkyrimSeamlessRequest(this.SkyrimSeamlessMemberMode == "promote" ? 8 : 9,target);
         this.SkyrimSeamlessShowCoop();
      }
      else if((action == "leave" && role == 2) || (action == "end" && role == 1))
      {
         this.SkyrimSeamlessPendingConfirm = action;
         this.ConfirmTextField.textAutoSize = "shrink";
         this.ConfirmTextField.SetText(action == "leave" ? "Leave the party? Your character keeps what you found." : "End the party? Everyone returns to their own main menu.");
         this.StartState(SystemPage.QUIT_CONFIRM_STATE);
      }
   }
   function SetShowMod(show)
'@ 'co-op list and action bridge'
Patch @'
      if(this._ShowModMenu && this.CategoryList.entryList && this.CategoryList.entryList.length > 0)
      {
         this.CategoryList.entryList.splice(SystemPage.MOD_MANAGER_BUTTON_INDEX,0,{text:"$CREATIONS"});
         this.CategoryList.InvalidateData();
      }
'@ @'
      var entries = this.SkyrimSeamlessSystemEntries == undefined ? this.CategoryList.entryList : this.SkyrimSeamlessSystemEntries;
      if(this._ShowModMenu && entries && entries.length > 0)
      {
         entries.splice(SystemPage.MOD_MANAGER_BUTTON_INDEX,0,{text:"$CREATIONS"});
         if(this.SkyrimSeamlessSystemEntries != undefined && this.SkyrimSeamlessSystemIndex >= SystemPage.MOD_MANAGER_BUTTON_INDEX)
            this.SkyrimSeamlessSystemIndex++;
         if(this.SkyrimSeamlessSystemEntries == undefined)
            this.CategoryList.InvalidateData();
      }
'@ 'preserve Creations during co-op'
Patch @'
         case SystemPage.QUIT_CONFIRM_STATE:
            gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
            gfx.io.GameDelegate.call("QuitToMainMenu",[]);
            this.bMenuClosing = true;
            break;
'@ @'
         case SystemPage.QUIT_CONFIRM_STATE:
            gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
            if(this.SkyrimSeamlessPendingConfirm != "")
            {
               this.SkyrimSeamlessRequest(this.SkyrimSeamlessPendingConfirm == "leave" ? 6 : 7);
               this.SkyrimSeamlessPendingConfirm = "";
               this.bMenuClosing = true;
            }
            else
            {
               gfx.io.GameDelegate.call("QuitToMainMenu",[]);
               this.bMenuClosing = true;
            }
            break;
'@ 'native yes confirmation'
Patch @'
   function onCancelPress()
   {
      var _loc2_ = true;
'@ @'
   function onCancelPress()
   {
      if(this.iCurrentState == SystemPage.SSM_COOP_STATE && this.SkyrimSeamlessMemberMode != "")
      {
         this.SkyrimSeamlessShowCoop();
         return true;
      }
      if(this.iCurrentState == SystemPage.SSM_COOP_STATE)
      {
         this.SkyrimSeamlessRestoreSystem();
         gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
         return true;
      }
      this.SkyrimSeamlessPendingConfirm = "";
      var _loc2_ = true;
'@ 'member back and confirmation cancellation'
Patch @'
      else if(this.iCurrentState == SystemPage.MAIN_STATE)
      {
         _loc3_ = event.index;
'@ @'
      else if(this.iCurrentState == SystemPage.SSM_COOP_STATE)
      {
         this.SkyrimSeamlessChoose(event.entry.ssmAction,event.entry.ssmId);
         return;
      }
      else if(this.iCurrentState == SystemPage.MAIN_STATE)
      {
         if(event.entry.text == "CO-OP")
         {
            this.SkyrimSeamlessShowCoop();
            return;
         }
         if(event.entry.text == "OPTIONS")
         {
            this.SkyrimSeamlessRequest(2);
            return;
         }
         _loc3_ = event.index;
'@ 'category action before Bethesda indexes'
Patch @'
   function onCategoryListPress(event)
   {
      if(!this.bRemapMode && !this.bMenuClosing && !this.bSavingSettings && this.iCurrentState != SystemPage.TRANSITIONING)
'@ @'
   function onCategoryListPress(event)
   {
      if(this.iCurrentState == SystemPage.SSM_COOP_STATE)
      {
         this.onCancelPress();
         return;
      }
      if(!this.bRemapMode && !this.bMenuClosing && !this.bSavingSettings && this.iCurrentState != SystemPage.TRANSITIONING)
'@ 'co-op list cancel input'
Patch @'
   function RefreshSystemButtons()
   {
      if(this._ShowModMenu)
'@ @'
   function RefreshSystemButtons()
   {
      if(this.SkyrimSeamlessSystemEntries != undefined)
         return;
      if(this._ShowModMenu)
'@ 'do not run positional save update on co-op list'
Patch @'
   function UpdatePermissions()
   {
      if(this._ShowModMenu)
'@ @'
   function UpdatePermissions()
   {
      if(this.SkyrimSeamlessSystemEntries != undefined)
         return;
      if(this._ShowModMenu)
'@ 'do not run positional permissions update on co-op list'
Patch @'
   function endPage()
   {
      this.CategoryList.removeEventListener("itemPress",this,"onCategoryButtonPress");
'@ @'
   function endPage()
   {
      this.SkyrimSeamlessRestoreSystem();
      this.CategoryList.removeEventListener("itemPress",this,"onCategoryButtonPress");
'@ 'restore System list on tab switch'
Patch @'
   function onLoad()
   {
      this.CategoryList.entryList.push({text:"$QUICKSAVE"});
'@ @'
   function onUnload()
   {
      this.SkyrimSeamlessRestoreSystem();
   }
   function onLoad()
   {
      this.CategoryList.entryList.push({text:"$QUICKSAVE"});
'@ 'restore System list on unload'
Patch @'
         case SystemPage.PC_QUIT_LIST_STATE:
            return this.PCQuitPanel;
'@ @'
         case SystemPage.PC_QUIT_LIST_STATE:
            return this.PCQuitPanel;
         case SystemPage.SSM_COOP_STATE:
            return this.PanelRect;
'@ 'submenu panel'
Patch @'
      this.CategoryList.disableSelection = aiNewState != SystemPage.MAIN_STATE;
'@ @'
      this.CategoryList.disableSelection = aiNewState != SystemPage.MAIN_STATE && aiNewState != SystemPage.SSM_COOP_STATE;
'@ 'submenu focus'
Patch @'
         case SystemPage.MAIN_STATE:
            gfx.managers.FocusHandler.instance.setFocus(this.CategoryList,0);
            break;
'@ @'
         case SystemPage.MAIN_STATE:
         case SystemPage.SSM_COOP_STATE:
            gfx.managers.FocusHandler.instance.setFocus(this.CategoryList,0);
            break;
'@ 'co-op category focus'

[IO.File]::WriteAllText($systemPage, $script:text, [Text.UTF8Encoding]::new($false))
$journalPage = Join-Path $scriptsRoot '__Packages\Quest_Journal.as'
$journalText = [IO.File]::ReadAllText($journalPage)
$journalText = Assert-Replace $journalText '   function RestoreSavedSettings(aiSavedTab, abTabsDisabled)' @'
   function SkyrimSeamlessShowSystem()
   {
      if(this.bTabsDisabled || this.iCurrentTab == Quest_Journal.PAGE_SYSTEM)
         return;
      this.PageArray[this.iCurrentTab].endPage();
      this.iCurrentTab = Quest_Journal.PAGE_SYSTEM;
      this.SwitchPageToFront(this.iCurrentTab,false);
      this.TabButtonGroup.setSelectedButton(this.TabButtonGroup.getButtonAt(this.iCurrentTab));
   }
   function RestoreSavedSettings(aiSavedTab, abTabsDisabled)
'@ 'select System tab with native page cleanup'
$journalText = Assert-Replace $journalText '      gfx.io.GameDelegate.call("CloseMenu",[this.iCurrentTab,this.QuestsFader.Page_mc.selectedQuestID,this.QuestsFader.Page_mc.selectedQuestInstance]);' @'
      this.SystemFader.Page_mc.SkyrimSeamlessRestoreSystem();
      gfx.io.GameDelegate.call("CloseMenu",[this.iCurrentTab,this.QuestsFader.Page_mc.selectedQuestID,this.QuestsFader.Page_mc.selectedQuestInstance]);
'@ 'restore System list on Journal close'
[IO.File]::WriteAllText($journalPage, $journalText, [Text.UTF8Encoding]::new($false))
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $ffdec -onerror abort -importScript $sourceSwf $output $scriptsRoot 2>&1 |
    ForEach-Object { "$_" } | Where-Object { $_ -match 'WARNING|ERROR' } | ForEach-Object { Write-Host "ffdec: $_" }
$ErrorActionPreference = $previousPreference
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output)) { throw 'Could not compile the patched quest_journal.swf.' }
if ($PreviewOnly) {
    Write-Host "Preview compiled: $output"
    Write-Host "Patched SHA-256: $((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash)"
    return
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $dataPath "SkyrimSEMultiplayerBackups\$stamp-native-journal-menu"
New-Item -ItemType Directory -Force -Path $backupDir,(Split-Path $destination) | Out-Null
if (Test-Path -LiteralPath $destination) {
    Copy-Item -LiteralPath $destination -Destination (Join-Path $backupDir 'quest_journal.swf')
}
Copy-Item -LiteralPath $output -Destination $destination -Force
Write-Host "Installed native CO-OP and OPTIONS pause menu: $destination"
Write-Host "Patched SHA-256: $((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash)"
Write-Host "Backup directory: $backupDir"
