Scriptname AIAgentMasterWheelEffect extends activemagiceffect  

Event OnEffectStart(Actor akTarget, Actor akCaster)
  Debug.Trace("[CHIM] Master Wheel spell was cast")
  
  String[] _modes = new String[4]
  _modes[0] = "ROLEPLAY"
  _modes[1] = "SETTINGS"
  _modes[2] = "MODE"
  _modes[3] = "SOULGAZE"
  ; _modes[4] = "SNQE"  ; AI Quests disabled
  
  String[] _label = new String[4]
  _label[0] = "Roleplay Wheel"
  _label[1] = "Settings Wheel"
  _label[2] = "Mode Wheel"
  _label[3] = "Soulgaze Wheel"
  ; _label[4] = "AI Quests"  ; AI Quests disabled

  UIExtensions.InitMenu("UIWheelMenu")
  int j = 0
  while j < _modes.length
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
    j = j + 1
  endwhile
  
  int ret = UIExtensions.OpenMenu("UIWheelMenu")
  String currentMode = _modes[ret]

  if (currentMode == "ROLEPLAY")
    OpenRoleplayWheel()
  elseif (currentMode == "SETTINGS")
    OpenSettingsWheel()
  elseif (currentMode == "MODE")
    OpenModeWheel()
  elseif (currentMode == "SOULGAZE")
    OpenSoulgazeWheel()
  ; elseif (currentMode == "SNQE")  ; AI Quests disabled
  ;   OpenSNQEWheel()
  endif
EndEvent

Function OpenRoleplayWheel()
  ObjectReference crosshairRef = Game.GetCurrentCrosshairRef()
  Actor leader = None
  String targetName = ""
  If (crosshairRef && crosshairRef.GetBaseObject() as ActorBase)
    targetName = (crosshairRef.GetBaseObject() as ActorBase).GetName()
    leader = crosshairRef as Actor
  EndIf
  
  String[] _modes = new String[8]
  _modes[0] = "Write Diary"
  _modes[1] = "GATHER"
  _modes[2] = "FOLLOW_NPC"
  _modes[3] = "UPDATE_NPC"
  _modes[4] = "WAIT"
  _modes[5] = "FOLLOW"
  _modes[6] = "HALT"
  _modes[7] = "RENAME"
  
  String[] _label = new String[8]
  _label[0] = "Write Diary"
  _label[1] = "Gather friends"
  _label[2] = "Follow NPC"
  _label[3] = "Update NPC"
  _label[4] = "Wait Here"
  _label[5] = "Follow Me"
  _label[6] = "Stop All AI"
  _label[7] = "Add to BgL"

  if leader
    if leader.GetrelationShipRank(Game.GetPlayer()) < 0
      _label[4] = "Force Wait"
      _label[5] = "Force Follow"
    elseif leader.GetrelationShipRank(Game.GetPlayer()) < 1
      _label[5] = "Force Follow"
    endif
  endif
  
  UIExtensions.InitMenu("UIWheelMenu")
  int j = 0
  while j < _modes.length
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
    j = j + 1
  endwhile
  
  int ret = UIExtensions.OpenMenu("UIWheelMenu")
  String currentMode = _modes[ret]
  
  if (currentMode == "Write Diary")
    If (targetName != "")
      Actor targetActor = crosshairRef as Actor
      If (targetActor)
        Debug.Notification("[CHIM] " + targetName + " is writing diary entry")
        AIAgentFunctions.requestMessageForActor("Please, update your diary","diary", targetActor.GetDisplayName())
      Else
        Debug.Notification("[CHIM] You must look at a target to generate a Diary Entry.")
      EndIf
    EndIf
  ElseIf (currentMode == "GATHER")
    AIAgentAIMind.GatherAround()
  ElseIf (currentMode == "FOLLOW_NPC")
    If (leader)
      Debug.Notification("[CHIM] Follow NPC feature (requires hotkey implementation)")
    Else
      Debug.Notification("[CHIM] You must look at a target to use this")
    EndIf
  ElseIf (currentMode == "UPDATE_NPC")
    If (leader)
      Debug.Trace("[CHIM] Updating dynamic profile for "+leader.GetDisplayName())
      AIAgentFunctions.logMessage(leader.GetDisplayName(),"updateprofiles_batch_async")
    Else
      Debug.Notification("[CHIM] You must look at a target to use this")
    EndIf
  elseif (currentMode == "WAIT")
    Actor targetActor = crosshairRef as Actor
    If (targetActor)
      AIagentAIMind.StartWait(leader)
    else
      Debug.Notification("[CHIM] You must look at a target to use this")
    endif
  elseif (currentMode == "FOLLOW")
    Actor targetActor = crosshairRef as Actor
    If (targetActor)
      AIagentAIMind.Follow(leader,Game.GetPlayer())
    else
      Debug.Notification("[CHIM] You must look at a target to use this")
    endif
  elseif (currentMode == "HALT")
    Debug.Notification("[CHIM] Stopping AI actions")
    ObjectReference crosshairRef2 = Game.GetCurrentCrosshairRef()
    Actor crActor = crosshairRef2 as Actor
    if (crActor) 
      AIAgentAIMind.StopCurrent(crActor)
    else  
      Actor[] actors=AIAgentFunctions.findAllNearbyAgents()
      int i = 0
      while i < actors.Length
        Actor akActor = actors[i]
        Debug.Trace("[CHIM] Stopping actor: " + akActor)
        AIAgentAIMind.StopCurrent(akActor)
        i += 1
      endWhile
    endif
  elseif (currentMode == "RENAME")
    If (leader)
      string originalname = leader.GetDisplayName()
      UIMenuBase menus=UIExtensions.GetMenu("UITextEntryMenu")
      string savedName=StorageUtil.GetStringValue(leader, "RenamedBuffer",leader.GetDisplayName())
      if savedName != "None"
        UIExtensions.SetMenuPropertyString("UITextEntryMenu","text",savedName)
      else
        UIExtensions.SetMenuPropertyString("UITextEntryMenu","text",originalname)
      endif
      UIExtensions.OpenMenu("UITextEntryMenu")
      string messageText = UIExtensions.GetMenuResultString("UITextEntryMenu")
      StorageUtil.SetStringValue(leader, "RenamedBuffer",None)
      UIExtensions.SetMenuPropertyString("UITextEntryMenu","text","")
      if (messageText != "")
        AIAgentFunctions.logMessage("chim_renamenpc@"+originalname+"@"+messageText+"@"+leader.GetFormId(),"setconf")
        StorageUtil.SetStringValue(leader,"forcedName",messageText)
      endif
    Else
      Debug.Notification("[CHIM] You must look at a target to use this")
    EndIf
  EndIf
EndFunction

Function OpenSettingsWheel()
  Actor leader = Game.GetCurrentCrosshairRef() as Actor
  
  if (leader)
    ; NPC-specific settings - profile assignment
    String[] _modes = new String[4]
    _modes[0] = "1"
    _modes[1] = "2"
    _modes[2] = "3"
    _modes[3] = "4"
    
    String[] _label = new String[4]
    _label[0] = "Profile 1"
    _label[1] = "Profile 2"
    _label[2] = "Profile 3"
    _label[3] = "Profile 4"
    
    UIExtensions.InitMenu("UIWheelMenu")
    int j = 0
    while j < _modes.length
      UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
      UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
      UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
      j = j + 1
    endwhile
    
    int ret = UIExtensions.OpenMenu("UIWheelMenu")
    String currentMode = _modes[ret]

    if (currentMode == "1")
      AIAgentFunctions.logMessageForActor("1","core_profile_assign",leader.GetDisplayName())
      Debug.Notification("[CHIM] Assigned Profile 1 to " + leader.GetDisplayName())
    elseif (currentMode == "2")
      AIAgentFunctions.logMessageForActor("2","core_profile_assign",leader.GetDisplayName())
      Debug.Notification("[CHIM] Assigned Profile 2 to " + leader.GetDisplayName())
    elseif (currentMode == "3")
      AIAgentFunctions.logMessageForActor("3","core_profile_assign",leader.GetDisplayName())
      Debug.Notification("[CHIM] Assigned Profile 3 to " + leader.GetDisplayName())
    elseif (currentMode == "4")
      AIAgentFunctions.logMessageForActor("4","core_profile_assign",leader.GetDisplayName())
      Debug.Notification("[CHIM] Assigned Profile 4 to " + leader.GetDisplayName())
    endif
  else
    ; Global settings - LLM model selection
    String[] _modes = new String[5]
    _modes[0] = "1"
    _modes[1] = "2"
    _modes[2] = "3"
    _modes[3] = "4"
    _modes[4] = "5"
    
    String[] _label = new String[5]
    _label[0] = "Standard LLM"
    _label[1] = "Fast LLM"
    _label[2] = "Powerful LLM"
    _label[3] = "Experimental LLM"
    _label[4] = "Focus Chat"
    
    UIExtensions.InitMenu("UIWheelMenu")
    int j = 0
    while j < _modes.length
      UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
      UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
      UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
      j = j + 1
    endwhile
    
    int ret = UIExtensions.OpenMenu("UIWheelMenu")
    String currentMode = _modes[ret]

    if (currentMode == "1")
      AIAgentFunctions.logMessage("chim_profile_model@1","setconf")
      Debug.Notification("[CHIM] Switched to Standard LLM")
    elseif (currentMode == "2")
      AIAgentFunctions.logMessage("chim_profile_model@2","setconf")
      Debug.Notification("[CHIM] Switched to Fast LLM")
    elseif (currentMode == "3")
      AIAgentFunctions.logMessage("chim_profile_model@3","setconf")
      Debug.Notification("[CHIM] Switched to Powerful LLM")
    elseif (currentMode == "4")
      AIAgentFunctions.logMessage("chim_profile_model@4","setconf")
      Debug.Notification("[CHIM] Switched to Experimental LLM")
    elseif (currentMode == "5")
      AIAgentFunctions.logMessage("chim_context_mode@1","setconf")
      Debug.Notification("[CHIM] Enabled Focus Chat")
    endif
  endif
EndFunction

Function OpenModeWheel()
  String[] _modes = new String[8]
  _modes[0] = "STANDARD"
  _modes[1] = "WHISPER"
  _modes[2] = "DIRECTOR"
  _modes[3] = "SPAWN"
  _modes[4] = "CHEATMODE"
  _modes[5] = "AUTOCHAT"
  _modes[6] = "INJECTION_LOG"
  _modes[7] = "INJECTION_CHAT"
  
  String[] _label = new String[8]
  _label[0] = "Standard Chat"
  _label[1] = "Whisper Chat"
  _label[2] = "Director Mode"
  _label[3] = "Spawn NPC"
  _label[4] = "Cheat Mode"
  _label[5] = "Auto Chat"
  _label[6] = "Inject Event"
  _label[7] = "Inject & Chat"
  
  UIExtensions.InitMenu("UIWheelMenu")
  int j = 0
  while j < _modes.length
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
    j = j + 1
  endwhile
  
  int ret = UIExtensions.OpenMenu("UIWheelMenu")
  String currentMode = _modes[ret]
  int currentModeIndex = ret
  
  StorageUtil.SetIntValue(None, "AIAgent_CurrentModeIndex", currentModeIndex)
  Debug.Notification("[CHIM] Mode: " + currentMode)
  AIAgentFunctions.logMessage("chim_mode@"+currentMode,"setconf")
  
  if (currentModeIndex == 1)
    Debug.Trace("[CHIM] Enabling intimacy bubble effect")
    AIAgentFunctions.setConf("_max_distance_inside",256,256,256)
    AIAgentFunctions.setConf("_max_distance_outside",256,256,256)
  else
    Debug.Trace("[CHIM] Disabling intimacy bubble effect")
    AIAgentFunctions.setConf("_max_distance_inside",1200,1200,1200)
    AIAgentFunctions.setConf("_max_distance_outside",2400,2400,2400)
  endif
EndFunction

Function OpenSoulgazeWheel()
  String[] _modes = new String[4]
  _modes[0] = "1"
  _modes[1] = "2"
  _modes[2] = "3"
  _modes[3] = "4"
  
  String[] _label = new String[4]
  _label[0] = "Soulgaze"
  _label[1] = "NPC Photo Zoom"
  _label[2] = "NPC Photo"
  _label[3] = "Just Upload"

  UIExtensions.InitMenu("UIWheelMenu")
  int j = 0
  while j < _modes.length
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
    j = j + 1
  endwhile
  
  int ret = UIExtensions.OpenMenu("UIWheelMenu")
  String currentMode = _modes[ret]

  if (currentMode == "1")
    int mode = AIAgentFunctions.get_conf_i("_sgmode")
    AIAgentSoulGazeEffect.Soulgaze(mode)
  elseif (currentMode == "2")
    int mode = AIAgentFunctions.get_conf_i("_sgmode")
    AIAgentSoulGazeEffect.SendProfilePicture(mode, true)
  elseif (currentMode == "3")
    int mode = AIAgentFunctions.get_conf_i("_sgmode")
    AIAgentSoulGazeEffect.SendProfilePicture(mode, false)
  elseif (currentMode == "4")
    AIAgentFunctions.logMessage("uploadscreen","soulgaze")
    Debug.Notification("[CHIM] Screen uploaded")
  endif
EndFunction

Function OpenSNQEWheel()
  String[] _modes = new String[3]
  _modes[0] = "1"
  _modes[1] = "2"
  _modes[2] = "3"
  
  String[] _label = new String[3]
  _label[0] = "Start/Continue"
  _label[1] = "End"
  _label[2] = "Clean"
  
  UIExtensions.InitMenu("UIWheelMenu")

  int j = 0
  while j < _modes.length
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionLabelText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu","optionText",j,_label[j])
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu","optionEnabled",j,true)
    j = j + 1
  endwhile
    
  int ret = UIExtensions.OpenMenu("UIWheelMenu")
  String currentMode = _modes[ret]

  if (currentMode == "1")
    AIAgentFunctions.logMessage("start","snqe")
  elseif (currentMode == "2")
    AIAgentFunctions.logMessage("end","snqe")
  elseif (currentMode == "3")
    AIAgentFunctions.logMessage("clean","snqe")  
  endif
EndFunction


