Scriptname AIAgentToggleModeEffect extends activemagiceffect  

Event OnEffectStart(Actor akTarget, Actor akCaster)
  Debug.Trace("[CHIM] Toggle Mode spell was cast")
  
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
  
  int j = 0
  UIExtensions.InitMenu("UIWheelMenu")
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
EndEvent
