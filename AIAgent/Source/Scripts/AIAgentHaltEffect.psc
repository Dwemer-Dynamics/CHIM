Scriptname AIAgentHaltEffect extends activemagiceffect  

Event OnEffectStart(Actor akTarget, Actor akCaster)
  Debug.Trace("[CHIM] Halt AI Actions spell was cast")
  
  Debug.Notification("[CHIM] Stopping AI actions")
  Actor[] actors = AIAgentFunctions.findAllNearbyAgents()
  
  int i = 0
  while i < actors.Length
    Actor akActor = actors[i]
    Debug.Trace("[CHIM] Stopping actor: " + akActor.GetDisplayName())
    AIAgentAIMind.StopCurrent(akActor)
    i += 1
  endWhile
  
  Debug.Trace("[CHIM] Halt AI Actions spell completed")
endEvent
