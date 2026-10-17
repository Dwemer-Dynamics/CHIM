Scriptname AIAgentFollowSafety Hidden

Bool Function IsValidFollowTarget(Actor npc, ObjectReference target) global
    return npc && target && npc != target
EndFunction

; Remove only CHIM's demonstrably broken self-follow override.
Bool Function RepairSelfFollow(Actor npc) global
    if (!npc)
        return false
    endif
    Package followPackage = Game.GetFormFromFile(0x01BC25, "AIAgent.esp") as Package
    Keyword moveTarget = Game.GetFormFromFile(0x021245, "AIAgent.esp") as Keyword
    if (!followPackage || !moveTarget || npc.GetCurrentPackage() != followPackage || npc.GetLinkedRef(moveTarget) != npc)
        return false
    endif
    ActorUtil.RemovePackageOverride(npc, followPackage)
    PO3_SKSEFunctions.SetLinkedRef(npc, None, moveTarget)
    Faction followFaction = Game.GetFormFromFile(0x01BC24, "AIAgent.esp") as Faction
    if (followFaction)
        npc.RemoveFromFaction(followFaction)
    endif
    npc.EvaluatePackage()
    Debug.Trace("[CHIM] Repaired invalid self-follow override for " + npc.GetDisplayName())
    return true
EndFunction

Function RepairLoadedSelfFollowers() global
    Actor[] actors = PO3_SKSEFunctions.GetActorsByProcessingLevel(0)
    if (!actors)
        return
    endif
    int i = 0
    while i < actors.Length
        RepairSelfFollow(actors[i])
        i += 1
    endwhile
EndFunction
