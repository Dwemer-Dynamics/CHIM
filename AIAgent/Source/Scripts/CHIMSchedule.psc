Scriptname CHIMSchedule Hidden

; Execute correlated appointment commands without resetting unrelated NPC packages.
Function Execute(Actor npc, String[] args) Global
    if args.Length != 6 || !npc || npc == Game.GetPlayer()
        return
    endif
    String runId = args[1]
    String token = args[2]
    String operation = args[3]
    String result = "invalid"
    ObjectReference marker = None
    Float issued = args[5] as Float
    Float now = Utility.GetCurrentGameTime()
    String owner = StorageUtil.GetStringValue(npc, "CHIM_ScheduleOwner", "")
    String closedKey = "CHIM_ScheduleClosed_" + runId
    String ownerKey = runId + ":" + token
    if AIAgentFunctions.getChimInteractionState() != 1 && operation != "release"
        result = "busy"
    elseif issued > now + 0.0001 || now - issued > 0.041667
        result = "stale"
    elseif operation == "release"
        StorageUtil.SetStringValue(npc, closedKey, token)
        if owner == ownerKey
            marker = StorageUtil.GetFormValue(npc, "CHIM_ScheduleMarker") as ObjectReference
            ; A later action may have replaced our link. Do not remove its packages.
            if npc.GetLinkedRef() == marker
                Package travel = Game.GetFormFromFile(0x01ABFE, "AIAgent.esp") as Package
                ActorUtil.RemovePackageOverride(npc, travel)
                Faction travelFaction = Game.GetFormFromFile(0x01A69C, "AIAgent.esp") as Faction
                Int oldRank = StorageUtil.GetIntValue(npc, "CHIM_ScheduleOldRank", -2)
                if oldRank == -2
                    npc.RemoveFromFaction(travelFaction)
                else
                    npc.SetFactionRank(travelFaction, oldRank)
                endif
                PO3_SKSEFunctions.SetLinkedRef(npc, StorageUtil.GetFormValue(npc, "CHIM_ScheduleOldLink") as ObjectReference)
                npc.EvaluatePackage()
            endif
            StorageUtil.UnsetStringValue(npc, "CHIM_ScheduleOwner")
        endif
        result = "released"
    elseif StorageUtil.GetStringValue(npc, closedKey, "") == token
        result = "stale"
    else
        Form target = Game.GetFormEx(AIAgentAIMind.HexToInt(args[4]))
        Location place = target as Location
        if place
            marker = AIAgentFunctions.getLocationCenterMarker(place, 0)
            if !marker
                marker = AIAgentFunctions.getWorldLocationMarkerFor(place)
            endif
        else
            marker = target as ObjectReference
        endif
        if marker && place && marker.GetCurrentLocation() != place
            marker = None
        endif
        ; Door references are not safe arrival points. Require a real location marker.
        if marker && marker.GetBaseObject() && marker.GetBaseObject().GetType() != 29 && !marker.IsDisabled()
            if operation == "validate"
                result = "validated"
            elseif npc.IsDead()
                result = "invalid"
            elseif npc.IsInCombat() || npc.IsInDialogueWithPlayer() || (owner != "" && owner != ownerKey)
                result = "busy"
            else
                Bool sameArea = npc.GetParentCell() == marker.GetParentCell()
                if !npc.IsInInterior() && !marker.IsInInterior()
                    sameArea = npc.GetWorldSpace() == marker.GetWorldSpace()
                endif
                Bool arrived = sameArea && npc.GetDistance(marker) <= 512.0
                if owner == "" && (operation == "travel" || operation == "ensure")
                    StorageUtil.SetStringValue(npc, "CHIM_ScheduleOwner", ownerKey)
                    StorageUtil.SetFormValue(npc, "CHIM_ScheduleMarker", marker)
                    StorageUtil.SetFormValue(npc, "CHIM_ScheduleOldLink", npc.GetLinkedRef())
                    Faction travelFaction = Game.GetFormFromFile(0x01A69C, "AIAgent.esp") as Faction
                    Int oldRank = -2
                    if npc.IsInFaction(travelFaction)
                        oldRank = npc.GetFactionRank(travelFaction)
                    endif
                    StorageUtil.SetIntValue(npc, "CHIM_ScheduleOldRank", oldRank)
                    npc.SetFactionRank(travelFaction, 1)
                    PO3_SKSEFunctions.SetLinkedRef(npc, marker)
                    Package travel = Game.GetFormFromFile(0x01ABFE, "AIAgent.esp") as Package
                    ActorUtil.AddPackageOverride(npc, travel, 100)
                    npc.EvaluatePackage()
                    owner = ownerKey
                endif
                if owner != ownerKey || npc.GetLinkedRef() != marker
                    result = "busy"
                elseif operation == "ensure" && !arrived
                    npc.MoveTo(marker)
                    if npc.GetDistance(marker) <= 512.0
                        result = "teleported"
                    endif
                elseif arrived
                    result = "arrived"
                else
                    result = "travelling"
                endif
            endif
        endif
    endif
    String markerId = "00000000"
    if marker
        markerId = AIAgentAIMind.DecToHex(marker.GetFormID())
    endif
    AIAgentFunctions.logMessage(runId + "/" + token + "/" + AIAgentAIMind.DecToHex(npc.GetFormID()) + "/" + operation + "/" + result + "/" + markerId, "util_npc_schedule")
EndFunction
