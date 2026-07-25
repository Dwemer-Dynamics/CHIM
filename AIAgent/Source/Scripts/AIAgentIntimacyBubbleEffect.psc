Scriptname AIAgentIntimacyBubbleEffect extends activemagiceffect  

Spell Property IntimacySpell  Auto  
MagicEffect Property IntimacyEffect  Auto  

int Property mdi auto
int Property mdo auto

Event OnEffectStart(Actor akTarget, Actor akCaster)
	Debug.Trace("[CHIM] Legacy close-mode compatibility effect selected persistent Close conversation mode")
	AIAgentFunctions.logMessage("chim_mode@CLOSE","setconf")
	Debug.Notification("[CHIM] Close conversation mode active")
EndEvent


Event OnEffectFinish(Actor akTarget, Actor akCaster)
	Debug.Trace("[CHIM] Legacy close-mode compatibility effect ended without changing conversation mode")
EndEvent


