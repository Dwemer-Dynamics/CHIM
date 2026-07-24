Scriptname AIAgentIntimacyBubbleEffect extends activemagiceffect  

Spell Property IntimacySpell  Auto  
MagicEffect Property IntimacyEffect  Auto  

int Property mdi auto
int Property mdo auto

Event OnEffectStart(Actor akTarget, Actor akCaster)
	Debug.Trace("[CHIM] Legacy intimacy effect selected persistent Intimate conversation mode")
	AIAgentFunctions.logMessage("chim_mode@INTIMATE","setconf")
	Debug.Notification("[CHIM] Intimate conversation mode active")
EndEvent


Event OnEffectFinish(Actor akTarget, Actor akCaster)
	Debug.Trace("[CHIM] Legacy intimacy effect ended without changing conversation mode")
EndEvent


