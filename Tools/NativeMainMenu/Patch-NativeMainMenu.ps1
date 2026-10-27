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
# CHARACTERS (owner 2026-10-01): a main menu entry that, in the live lobby, turns the menu into a character picker at
# the fire. Input comes through the menu's own navigation events, so the d-pad/stick, keyboard and mouse all work;
# the lobby (LobbyService) reads _root.SkyrimSeamlessCharactersOpen / SkyrimSeamlessCharacterStep and writes
# _root.SSMCharName, SSMCharDetail and SSMCharCount.
$text = Assert-Replace $text `
    '   static var SKYRIM_SEAMLESS_OPTIONS_INDEX = 14;' `
    "   static var SKYRIM_SEAMLESS_OPTIONS_INDEX = 14;`r`n   static var SKYRIM_SEAMLESS_CHARACTERS_INDEX = 15;`r`n   var SkyrimSeamlessCharMode = false;`r`n   var SkyrimSeamlessMotdWasVisible = true;`r`n   var SkyrimSeamlessBarWasVisible = false;`r`n   var SkyrimSeamlessBarListening = false;`r`n   var SkyrimSeamlessPendingSaves = undefined;`r`n   var SkyrimSeamlessDeleteConfirm = false;`r`n   var SkyrimSeamlessBarShown = false;`r`n   static var SKYRIM_SEAMLESS_READY_INDEX = 16;`r`n   static var SKYRIM_SEAMLESS_LEAVE_INDEX = 17;`r`n   var SkyrimSeamlessMenuArgs = undefined;`r`n   var SkyrimSeamlessRoleStamp = 0;" `
    'characters index declaration'
$text = Assert-Replace $text `
    '      this.MainList.entryList.push({text:"CO-OP",index:StartMenu.SKYRIM_SEAMLESS_COOP_INDEX,disabled:false,showIcon:false});' `
    "      this.MainList.entryList.push({text:`"CHARACTERS`",index:StartMenu.SKYRIM_SEAMLESS_CHARACTERS_INDEX,disabled:false,showIcon:false});`r`n      this.MainList.entryList.push({text:`"CO-OP`",index:StartMenu.SKYRIM_SEAMLESS_COOP_INDEX,disabled:false,showIcon:false});" `
    'characters entry'
$text = Assert-Replace $text `
    '            case StartMenu.SKYRIM_SEAMLESS_COOP_INDEX:' `
    "            case StartMenu.SKYRIM_SEAMLESS_READY_INDEX:`r`n               _root.SkyrimSeamlessReadyRequested = 1;`r`n               gfx.io.GameDelegate.call(`"PlaySound`",[`"UIMenuOK`"]);`r`n               return;`r`n            case StartMenu.SKYRIM_SEAMLESS_LEAVE_INDEX:`r`n               _root.SkyrimSeamlessLeaveRequested = 1;`r`n               gfx.io.GameDelegate.call(`"PlaySound`",[`"UIMenuCancel`"]);`r`n               return;`r`n            case StartMenu.SKYRIM_SEAMLESS_CHARACTERS_INDEX:`r`n               this.SkyrimSeamlessEnterCharacters();`r`n               gfx.io.GameDelegate.call(`"PlaySound`",[`"UIMenuOK`"]);`r`n               return;`r`n            case StartMenu.SKYRIM_SEAMLESS_COOP_INDEX:" `
    'characters selection'
$beforeInput = @'
   function handleInput(details, pathToFocus)
   {
      var _loc5_;
      var _loc4_;
      var _loc3_;
'@ -replace "`n", "`r`n"
$afterInput = @'
   function handleInput(details, pathToFocus)
   {
      var _loc5_;
      var _loc4_;
      var _loc3_;
      if(this.SkyrimSeamlessCharMode)
      {
         if(Shared.GlobalFunc.IsKeyPressed(details))
         {
            if(details.navEquivalent == gfx.ui.NavigationCode.LEFT || details.navEquivalent == gfx.ui.NavigationCode.GAMEPAD_L1)
            {
               _root.SkyrimSeamlessCharacterStep = -1;
               gfx.io.GameDelegate.call("PlaySound",["UIMenuFocus"]);
            }
            else if(details.navEquivalent == gfx.ui.NavigationCode.RIGHT || details.navEquivalent == gfx.ui.NavigationCode.GAMEPAD_R1)
            {
               _root.SkyrimSeamlessCharacterStep = 1;
               gfx.io.GameDelegate.call("PlaySound",["UIMenuFocus"]);
            }
            else if(_root.SSMPartyCount > 0 && (details.code == 80 || details.navEquivalent == gfx.ui.NavigationCode.GAMEPAD_R2))
            {
               // The party's characters have their own key (owner 2026-10-02: "seperate buttons/keys").
               _root.SkyrimSeamlessPartyStep = 1;
               gfx.io.GameDelegate.call("PlaySound",["UIMenuFocus"]);
            }
            else if(details.navEquivalent == gfx.ui.NavigationCode.GAMEPAD_Y || details.code == 84)
            {
               this.SkyrimSeamlessOpenSaves();
            }
            else if(details.navEquivalent == gfx.ui.NavigationCode.GAMEPAD_X || details.code == 88)
            {
               this.SkyrimSeamlessAskDelete();
            }
            else if(details.navEquivalent == gfx.ui.NavigationCode.ENTER)
            {
               // Another player's character is only looked at: nothing to choose.
               if(_root.SSMCharRemote == 1)
               {
                  gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
                  return undefined;
               }
               this.SkyrimSeamlessLeaveCharacters();
               gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
            }
            else if(details.navEquivalent == gfx.ui.NavigationCode.TAB)
            {
               this.SkyrimSeamlessLeaveCharacters();
               gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
            }
         }
         return true;
      }
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text $beforeInput $afterInput 'characters input'
$text = Assert-Replace $text `
    "   function SkyrimSeamlessWatchLayout()`r`n   {`r`n" `
    "   function SkyrimSeamlessWatchLayout()`r`n   {`r`n      this.SkyrimSeamlessCharactersTick();`r`n      if(_root.SkyrimSeamlessRoleStamp != undefined && _root.SkyrimSeamlessRoleStamp != this.SkyrimSeamlessRoleStamp && this.SkyrimSeamlessMenuArgs != undefined)`r`n      {`r`n         this.SkyrimSeamlessRoleStamp = _root.SkyrimSeamlessRoleStamp;`r`n         this.setupMainMenu.apply(this,this.SkyrimSeamlessMenuArgs);`r`n      }`r`n" `
    'characters tick'
$charFunctions = @'
   // The list for this player's place in a party (LobbyService TickPartyRole: 0 solo, 1 host, 2 guest). A guest gets
   // no CONTINUE, NEW, LOAD or CREATIONS (the host starts the party) but READY and LEAVE PARTY; the host's CONTINUE
   // and NEW start everyone together, and the party loads no other save.
   function SkyrimSeamlessApplyRole()
   {
      // Solo (role 0) loses only LOAD: CHARACTERS > SAVES opens the same Load screen on the chosen character (owner
      // 2026-10-02: one way to a save, not two).
      var role = Number(_root.SkyrimSeamlessRole);
      var kept = [];
      var i = 0;
      while(i < this.MainList.entryList.length)
      {
         var entry = this.MainList.entryList[i];
         var index = entry.index;
         if(role != 1 && role != 2)
         {
            if(index != StartMenu.LOAD_INDEX)
            {
               kept.push(entry);
            }
         }
         else if(role == 2)
         {
            if(index == StartMenu.SKYRIM_SEAMLESS_CHARACTERS_INDEX || index == StartMenu.SKYRIM_SEAMLESS_COOP_INDEX || index == StartMenu.SKYRIM_SEAMLESS_OPTIONS_INDEX || index == StartMenu.QUIT_INDEX)
            {
               kept.push(entry);
            }
         }
         else if(index != StartMenu.LOAD_INDEX && index != StartMenu.CREATION_CLUB_INDEX && index != StartMenu.MOD_INDEX)
         {
            // Greyed out until every other member is ready (owner 2026-10-02: pressed early, the menu faded to black and
            // the start was refused, leaving the host on a black screen).
            if(index == StartMenu.CONTINUE_INDEX)
            {
               entry.text = "CONTINUE TOGETHER";
               entry.disabled = _root.SkyrimSeamlessOthersReady != 1;
            }
            else if(index == StartMenu.NEW_INDEX)
            {
               entry.text = "NEW GAME TOGETHER";
               entry.disabled = _root.SkyrimSeamlessOthersReady != 1;
            }
            kept.push(entry);
         }
         i++;
      }
      if(role == 2)
      {
         kept.unshift({text:(_root.SkyrimSeamlessReady == 1 ? "CANCEL READY" : "READY"),index:StartMenu.SKYRIM_SEAMLESS_READY_INDEX,disabled:false,showIcon:false});
         var quit = kept.length;
         i = 0;
         while(i < kept.length)
         {
            if(kept[i].index == StartMenu.QUIT_INDEX)
            {
               quit = i;
            }
            i++;
         }
         kept.splice(quit,0,{text:"LEAVE PARTY",index:StartMenu.SKYRIM_SEAMLESS_LEAVE_INDEX,disabled:false,showIcon:false});
      }
      this.MainList.entryList.splice(0,this.MainList.entryList.length);
      i = 0;
      while(i < kept.length)
      {
         this.MainList.entryList.push(kept[i]);
         i++;
      }
      this.MainList.RestoreScrollPosition(0,false);
   }
   function SkyrimSeamlessEnterCharacters()
   {
      this.SkyrimSeamlessCharMode = true;
      this.MainList._visible = false;
      this.MainListHolder._visible = false;
      this.SkyrimSeamlessMotdWasVisible = this.MessageOfTheDay_mc._visible;
      this.MessageOfTheDay_mc._visible = false;
      this.Logo_mc._visible = false;
      _root.SkyrimSeamlessCharactersOpen = 1;
      this.SkyrimSeamlessCharacterButtons(true);
      this.SkyrimSeamlessCharactersTick();
   }
   function SkyrimSeamlessLeaveCharacters()
   {
      this.SkyrimSeamlessCharMode = false;
      this.MainList._visible = true;
      this.MainListHolder._visible = true;
      this.MessageOfTheDay_mc._visible = this.SkyrimSeamlessMotdWasVisible != false;
      this.Logo_mc._visible = true;
      _root.SkyrimSeamlessCharactersOpen = 0;
      this.SkyrimSeamlessCharacterButtons(false);
      this.SkyrimSeamlessCharactersTick();
   }
   // SAVES: Skyrim's own Load screen (its character list, then that character's saves, with Load and Delete and their
   // confirmations), opened with the shown character already picked.
   function SkyrimSeamlessOpenSaves()
   {
      if(_root.SSMCharCanBrowse != 1)
      {
         gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
         return undefined;
      }
      this.SkyrimSeamlessPendingSaves = String(_root.SSMCharRawName);
      this.SkyrimSeamlessLeaveCharacters();
      // The menu's cursor on CONTINUE beside the character's saves (owner 2026-10-01), not left on CHARACTERS.
      this.SkyrimSeamlessSelectNativeEntry(StartMenu.CONTINUE_INDEX);
      gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
      this.SaveLoadListHolder.isSaving = false;
      this.RequestCharacterListLoad();
   }
   // DELETE: Skyrim's own confirmation panel (MAIN_CONFIRM_STATE, as NEW asks), then back to the character screen.
   function SkyrimSeamlessAskDelete()
   {
      if(_root.SSMCharHasSaves != 1)
      {
         gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
         return undefined;
      }
      this.SkyrimSeamlessDeleteConfirm = true;
      this.SkyrimSeamlessCharMode = false;
      this.SkyrimSeamlessCharacterButtons(false);
      this.SkyrimSeamlessCharactersTick();
      gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
      this.ShowConfirmScreen("You are about to delete all save data for " + String(_root.SSMCharName) + ". Do you want to continue?");
   }
   function SkyrimSeamlessResumeCharacters()
   {
      this.SkyrimSeamlessCharMode = true;
      this.MainList._visible = false;
      this.MainListHolder._visible = false;
      this.MessageOfTheDay_mc._visible = false;
      this.Logo_mc._visible = false;
      this.SkyrimSeamlessCharacterButtons(true);
      this.SkyrimSeamlessCharactersTick();
   }
   function SkyrimSeamlessPickPendingCharacter()
   {
      var wanted = this.SkyrimSeamlessPendingSaves;
      this.SkyrimSeamlessPendingSaves = undefined;
      if(wanted == undefined || this.strCurrentState != StartMenu.CHARACTER_SELECTION_STATE)
      {
         return undefined;
      }
      var list = this.SaveLoadListHolder.List_mc;
      // The playthrough's ID, as the list's entries carry it; the name only for co-op checkpoints (no ID).
      var wantedId = Number(_root.SSMCharId);
      var i = 0;
      if(wantedId > 0)
      {
         while(i < list.entryList.length)
         {
            if(Number(list.entryList[i].id) == wantedId)
            {
               list.selectedIndex = i;
               this.SaveLoadListHolder.onSaveLoadItemPress({});
               return undefined;
            }
            i++;
         }
      }
      var lower = wanted.toLowerCase();
      i = 0;
      while(i < list.entryList.length)
      {
         var text = String(list.entryList[i].text).toLowerCase();
         // The list tags names ("Host [Modded]") and cuts long ones ("...").
         var tag = text.indexOf(" [");
         if(tag > 0)
         {
            text = text.substr(0,tag);
         }
         if(text.substr(text.length - 3) == "...")
         {
            text = text.substr(0,text.length - 3);
         }
         if(text == lower || (text.length >= 17 && lower.indexOf(text) == 0))
         {
            list.selectedIndex = i;
            this.SaveLoadListHolder.onSaveLoadItemPress({});
            return undefined;
         }
         i++;
      }
   }
   function SkyrimSeamlessCharacterButtons(aShow)
   {
      var bar = this._BottomButtons_mc;
      if(bar == undefined)
      {
         return undefined;
      }
      if(!this.SkyrimSeamlessBarListening)
      {
         this.SkyrimSeamlessBarListening = true;
         bar.addEventListener(BottomButtons.BUTTON_CLICKED,Shared.Proxy.create(this,this.SkyrimSeamlessCharacterButtonClick));
      }
      if(aShow)
      {
         if(!this.SkyrimSeamlessBarShown)
         {
            this.SkyrimSeamlessBarWasVisible = bar._visible;
         }
         this.SkyrimSeamlessBarShown = true;
         var buttons = [];
         if(_root.SSMCharCount > 1)
         {
            buttons.push({PCArt:"Left",XBoxArt:"360_LB",PS3Art:"PS3_LB",NXArt:"NX_LB",Label:"PREVIOUS",KeyCode:37,SSMAction:"previous"});
            buttons.push({PCArt:"Right",XBoxArt:"360_RB",PS3Art:"PS3_RB",NXArt:"NX_RB",Label:"NEXT",KeyCode:39,SSMAction:"next"});
         }
         if(_root.SSMPartyCount > 0)
         {
            buttons.push({PCArt:"P",XBoxArt:"360_RT",PS3Art:"PS3_RT",NXArt:"NX_RT",Label:"PARTY",KeyCode:80,SSMAction:"party"});
         }
         if(_root.SSMCharRemote != 1)
         {
            buttons.push({PCArt:"Enter",XBoxArt:"360_A",PS3Art:"PS3_A",NXArt:"NX_A",Label:"CHOOSE",KeyCode:13,SSMAction:"choose"});
         }
         if(_root.SSMCharCanBrowse == 1)
         {
            buttons.push({PCArt:"T",XBoxArt:"360_Y",PS3Art:"PS3_Y",NXArt:"NX_X",Label:"SAVES",KeyCode:84,SSMAction:"saves"});
         }
         if(_root.SSMCharHasSaves == 1)
         {
            buttons.push({PCArt:"X",XBoxArt:"360_X",PS3Art:"PS3_X",NXArt:"NX_Y",Label:"DELETE",KeyCode:88,SSMAction:"delete"});
         }
         buttons.push({PCArt:"Esc",XBoxArt:"360_B",PS3Art:"PS3_B",NXArt:"NX_B",Label:"BACK",KeyCode:9,SSMAction:"back"});
         bar.SetPlatform(this.iPlatform,this.PS3Switch);
         bar.SetButtons([]);
         bar.SetButtons(buttons);
         bar.Hide(false);
         bar._visible = true;
         // The labels size themselves a frame or two later: lay the bar out once they have.
         setTimeout(Shared.Proxy.create(this,this.SkyrimSeamlessLayoutBar),120);
         setTimeout(Shared.Proxy.create(this,this.SkyrimSeamlessLayoutBar),400);
      }
      else
      {
         bar.SetButtons([]);
         bar.Hide(true);
         bar._visible = this.SkyrimSeamlessBarWasVisible == true;
         this.SkyrimSeamlessBarShown = false;
      }
   }
   // The bar's own layout sizes each button by its 177-wide hit area, not its icon and label (2026-10-01 measurement:
   // labels 31-65 wide), which spread six buttons over the whole screen. Each button is cut to its content and the row
   // is centred with the bar's own spacing.
   function SkyrimSeamlessLayoutBar()
   {
      var bar = this._BottomButtons_mc;
      if(bar == undefined || !this.SkyrimSeamlessBarShown)
      {
         return undefined;
      }
      var buttons = bar._buttons;
      var widths = [];
      var total = 0;
      var count = 0;
      var i = 0;
      while(i < buttons.length)
      {
         var button = buttons[i];
         var width = 0;
         if(button._visible)
         {
            width = Math.ceil(button.textField._x + Math.max(button.textField._width,button.textField.textWidth + 6) + 6);
            if(button.border != undefined)
            {
               button.border._width = width;
            }
            total += width;
            count++;
         }
         widths.push(width);
         i++;
      }
      if(count == 0)
      {
         return undefined;
      }
      var spacing = 34;
      // The end caps of the frame take more room on the right than the bar's side margin (2026-10-01 capture).
      total += (count - 1) * spacing + bar._buttonBgSide * 2 + 24;
      var first = 0;
      while(first < buttons.length && !buttons[first]._visible)
      {
         first++;
      }
      var y = bar.bg_mc._y + (bar.bg_mc._height - buttons[first]._height) / 2;
      // Centred under the character's name (owner 2026-10-02: the box was off centre): the same visible-stage centre
      // the name uses, taken into the bar's own coordinates (the bar is anchored with its own offset and scale).
      var rect = Stage.visibleRect;
      var centre = {x:rect.x + rect.width / 2,y:0};
      _root.localToGlobal(centre);
      bar.globalToLocal(centre);
      var x = Math.round(centre.x - total / 2);
      bar.bg_mc._x = x;
      bar.bg_mc._width = total;
      x += bar._buttonBgSide;
      i = 0;
      while(i < buttons.length)
      {
         if(buttons[i]._visible)
         {
            var bounds = buttons[i].getBounds(buttons[i]);
            buttons[i]._x = x - bounds.xMin;
            buttons[i]._y = y;
            x += widths[i] + spacing;
         }
         i++;
      }
      // For the test bench: the box's centre and the name's, both in _root coordinates.
      var box = {x:bar.bg_mc._x + bar.bg_mc._width / 2,y:0};
      bar.localToGlobal(box);
      _root.globalToLocal(box);
      _root.SSMBarCentre = box.x;
      _root.SSMNameCentre = rect.x + rect.width / 2;
   }
   function SkyrimSeamlessCharacterButtonClick(event)
   {
      if(!this.SkyrimSeamlessCharMode)
      {
         return undefined;
      }
      if(event.data.SSMAction == "previous" || event.data.SSMAction == "next")
      {
         _root.SkyrimSeamlessCharacterStep = event.data.SSMAction == "next" ? 1 : -1;
         gfx.io.GameDelegate.call("PlaySound",["UIMenuFocus"]);
      }
      else if(event.data.SSMAction == "party")
      {
         _root.SkyrimSeamlessPartyStep = 1;
         gfx.io.GameDelegate.call("PlaySound",["UIMenuFocus"]);
      }
      else if(event.data.SSMAction == "delete")
      {
         this.SkyrimSeamlessAskDelete();
      }
      else if(event.data.SSMAction == "saves")
      {
         this.SkyrimSeamlessOpenSaves();
      }
      else if(event.data.SSMAction == "choose")
      {
         this.SkyrimSeamlessLeaveCharacters();
         gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
      }
      else if(event.data.SSMAction == "back")
      {
         this.SkyrimSeamlessLeaveCharacters();
         gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
      }
   }
   function SkyrimSeamlessCharacterField(aName, aDepth, aFont, aSize, aColor, aSpacing)
   {
      var field = _root.createTextField(aName,aDepth,0,0,900,aSize + 18);
      field.embedFonts = true;
      field.selectable = false;
      var format = new TextFormat();
      format.font = aFont;
      format.size = aSize;
      format.color = aColor;
      format.align = "center";
      format.letterSpacing = aSpacing;
      field.setNewTextFormat(format);
      return field;
   }
   // A thin line that fades out at both ends, as Skyrim draws under its location and quest banners.
   function SkyrimSeamlessCharacterRule(aName, aDepth, aWidth)
   {
      var rule = _root.createEmptyMovieClip(aName,aDepth);
      var matrix = {matrixType:"box",x:- aWidth / 2,y:0,w:aWidth,h:1,r:0};
      rule.beginGradientFill("linear",[16777215,16777215,16777215],[0,70,0],[0,127,255],matrix);
      rule.moveTo(- aWidth / 2,0);
      rule.lineTo(aWidth / 2,0);
      rule.lineTo(aWidth / 2,1);
      rule.lineTo(- aWidth / 2,1);
      rule.lineTo(- aWidth / 2,0);
      rule.endFill();
      return rule;
   }
   // A small solid triangle, as Skyrim's menus point left and right.
   function SkyrimSeamlessCharacterArrow(aName, aDepth, aStep)
   {
      var arrow = _root.createEmptyMovieClip(aName,aDepth);
      arrow.beginFill(0,0);
      arrow.moveTo(-28,-28);
      arrow.lineTo(28,-28);
      arrow.lineTo(28,28);
      arrow.lineTo(-28,28);
      arrow.lineTo(-28,-28);
      arrow.endFill();
      arrow.beginFill(16777215,100);
      arrow.moveTo(7 * aStep,0);
      arrow.lineTo(-5 * aStep,-10);
      arrow.lineTo(-5 * aStep,10);
      arrow.lineTo(7 * aStep,0);
      arrow.endFill();
      arrow._alpha = 55;
      arrow.step = aStep;
      arrow.onRollOver = function()
      {
         this._alpha = 100;
      };
      arrow.onRollOut = function()
      {
         this._alpha = 55;
      };
      arrow.onRelease = function()
      {
         _root.SkyrimSeamlessCharacterStep = this.step;
         gfx.io.GameDelegate.call("PlaySound",["UIMenuFocus"]);
      };
      return arrow;
   }
   function SkyrimSeamlessCharactersTick()
   {
      var show = this.SkyrimSeamlessCharMode;
      if(show && _root.SSMCharName_tf == undefined)
      {
         this.SkyrimSeamlessCharacterField("SSMCharName_tf",16010,"$EverywhereMediumFont",34,16777215,4);
         this.SkyrimSeamlessCharacterField("SSMCharDetail_tf",16011,"$EverywhereMediumFont",16,12632256,2);
         this.SkyrimSeamlessCharacterRule("SSMCharRuleTop_mc",16012,520);
         this.SkyrimSeamlessCharacterRule("SSMCharRuleBottom_mc",16013,520);
         this.SkyrimSeamlessCharacterArrow("SSMCharLeft_mc",16014,-1);
         this.SkyrimSeamlessCharacterArrow("SSMCharRight_mc",16015,1);
         // Where in the list this character is ("3 / 64"), small and grey above the banner, as Skyrim's counts read.
         this.SkyrimSeamlessCharacterField("SSMCharCounter_tf",16016,"$EverywhereMediumFont",15,12632256,2);
      }
      if(_root.SSMCharName_tf == undefined)
      {
         return undefined;
      }
      var arrows = show && _root.SSMCharCount > 1;
      _root.SSMCharName_tf._visible = show;
      _root.SSMCharDetail_tf._visible = show;
      _root.SSMCharRuleTop_mc._visible = show;
      _root.SSMCharRuleBottom_mc._visible = show;
      _root.SSMCharLeft_mc._visible = arrows;
      _root.SSMCharRight_mc._visible = arrows;
      _root.SSMCharCounter_tf._visible = show && (_root.SSMCharCount > 1 || _root.SSMCharRemote == 1);
      if(!show)
      {
         return undefined;
      }
      if(_root.SSMCharRefresh == 1)
      {
         _root.SSMCharRefresh = 0;
         this.SkyrimSeamlessCharacterButtons(true);
      }
      // Every frame: the glyphs change with the last device used (keyboard keys are wider than gamepad buttons).
      this.SkyrimSeamlessLayoutBar();
      var rect = Stage.visibleRect;
      var cx = rect.x + rect.width / 2;
      var bottom = rect.y + rect.height;
      _root.SSMCharRuleTop_mc._x = cx;
      _root.SSMCharRuleTop_mc._y = bottom - 200;
      _root.SSMCharName_tf._x = cx - 450;
      _root.SSMCharName_tf._y = bottom - 192;
      _root.SSMCharDetail_tf._x = cx - 450;
      _root.SSMCharDetail_tf._y = bottom - 144;
      _root.SSMCharRuleBottom_mc._x = cx;
      _root.SSMCharRuleBottom_mc._y = bottom - 112;
      _root.SSMCharLeft_mc._x = cx - 300;
      _root.SSMCharLeft_mc._y = bottom - 160;
      _root.SSMCharRight_mc._x = cx + 300;
      _root.SSMCharRight_mc._y = bottom - 160;
      _root.SSMCharCounter_tf._x = cx - 450;
      _root.SSMCharCounter_tf._y = bottom - 232;
      var counter = _root.SSMCharIndex == undefined || _root.SSMCharCount == undefined ? "" : (_root.SSMCharRemote == 1 ? "PARTY  " : "") + String(_root.SSMCharIndex) + " / " + String(_root.SSMCharCount);
      if(_root.SSMCharCounter_tf.text != counter)
      {
         _root.SSMCharCounter_tf.text = counter;
      }
      var name = _root.SSMCharName == undefined ? "" : String(_root.SSMCharName);
      var detail = _root.SSMCharDetail == undefined ? "" : String(_root.SSMCharDetail);
      if(_root.SSMCharName_tf.text != name)
      {
         _root.SSMCharName_tf.text = name;
      }
      if(_root.SSMCharDetail_tf.text != detail)
      {
         _root.SSMCharDetail_tf.text = detail;
      }
   }
   function SkyrimSeamlessSelectNativeEntry(aEntryIndex)
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text `
    '   function SkyrimSeamlessSelectNativeEntry(aEntryIndex)' `
    $charFunctions `
    'characters functions'
$pickBefore = @'
         this.ShowCharacterSelectionHint(false);
         gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
         this.StartState(StartMenu.CHARACTER_SELECTION_STATE);
'@ -replace "`n", "`r`n"
$pickAfter = @'
         this.ShowCharacterSelectionHint(false);
         gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
         this.StartState(StartMenu.CHARACTER_SELECTION_STATE);
         if(this.SkyrimSeamlessPendingSaves != undefined)
         {
            setTimeout(Shared.Proxy.create(this,this.SkyrimSeamlessPickPendingCharacter),400);
         }
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text $pickBefore $pickAfter 'characters saves pick'
$acceptBefore = @'
   function onAcceptPress()
   {
      switch(this.strCurrentState)
      {
'@ -replace "`n", "`r`n"
$acceptAfter = @'
   function onAcceptPress()
   {
      if(this.SkyrimSeamlessDeleteConfirm && this.strCurrentState == StartMenu.MAIN_CONFIRM_STATE)
      {
         this.SkyrimSeamlessDeleteConfirm = false;
         _root.SkyrimSeamlessDeleteRequested = 1;
         gfx.io.GameDelegate.call("PlaySound",["UIMenuOK"]);
         this.EndState();
         this.SkyrimSeamlessResumeCharacters();
         return undefined;
      }
      switch(this.strCurrentState)
      {
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text $acceptBefore $acceptAfter 'characters delete accept'
$cancelBefore = @'
   function onCancelPress()
   {
      switch(this.strCurrentState)
      {
'@ -replace "`n", "`r`n"
$cancelAfter = @'
   function onCancelPress()
   {
      if(this.SkyrimSeamlessDeleteConfirm && this.strCurrentState == StartMenu.MAIN_CONFIRM_STATE)
      {
         this.SkyrimSeamlessDeleteConfirm = false;
         gfx.io.GameDelegate.call("PlaySound",["UIMenuCancel"]);
         this.EndState();
         this.SkyrimSeamlessResumeCharacters();
         return undefined;
      }
      switch(this.strCurrentState)
      {
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text $cancelBefore $cancelAfter 'characters delete cancel'
$setupBefore = @'
   function setupMainMenu()
   {
'@ -replace "`n", "`r`n"
$setupAfter = @'
   function setupMainMenu()
   {
      this.SkyrimSeamlessMenuArgs = arguments;
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text $setupBefore $setupAfter 'role: keep the list arguments'
$drawBefore = @'
      this.MainList.InvalidateData();
      this._NeedsLoginScreen = !arguments[_loc12_];
'@ -replace "`n", "`r`n"
$drawAfter = @'
      this.SkyrimSeamlessApplyRole();
      this.MainList.InvalidateData();
      this._NeedsLoginScreen = !arguments[_loc12_];
'@ -replace "`n", "`r`n"
$text = Assert-Replace $text $drawBefore $drawAfter 'role: filter the list'
[IO.File]::WriteAllText($startMenuScript, $text, [Text.UTF8Encoding]::new($false))

# ffdec reports warnings on stderr (a long class header, harmless); success is its exit code and the output file.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $ffdec -onerror abort -importScript $sourceSwf $output $scriptsRoot 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match 'WARNING|ERROR' } | ForEach-Object { Write-Host "ffdec: $_" }
$ErrorActionPreference = $previousPreference
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output)) { throw 'Could not compile the patched startmenu.swf.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $dataPath "SkyrimSEMultiplayerBackups\$stamp-native-main-menu"
New-Item -ItemType Directory -Force -Path $backupDir,(Split-Path $destination) | Out-Null
if (Test-Path -LiteralPath $destination) {
    Copy-Item -LiteralPath $destination -Destination (Join-Path $backupDir 'startmenu.swf')
}
Copy-Item -LiteralPath $output -Destination $destination -Force

$outputHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
Write-Host "Installed native CHARACTERS, CO-OP and OPTIONS menu: $destination"
Write-Host "Patched SHA-256: $outputHash"
Write-Host "Backup directory: $backupDir"
