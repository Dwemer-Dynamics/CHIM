Scriptname AIAgentSoulGazerEffect extends activemagiceffect  



Event OnEffectStart(Actor akTarget, Actor akCaster)
  Debug.Trace("Magic effect was started on " + akTarget)
  ;Debug.Notification("Magic effect was started on " + akTarget)
  
  int mode = AIAgentFunctions.get_conf_i("_sgmode");
  AIAgentSoulGazeEffect.Soulgaze(mode);
  
endEvent


