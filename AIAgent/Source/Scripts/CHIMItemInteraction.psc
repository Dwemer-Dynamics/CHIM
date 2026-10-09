Scriptname CHIMItemInteraction Hidden

; Native session checks prevent queued Papyrus work from crossing a load or cancellation.
Bool Function CanExecute(String requestId, Int step) Global Native
Function Complete(String requestId, Int step, String status, String detail) Global Native
Function FinishInjury(String requestId, Int step, Float healthBefore) Global Native
Bool Function ClaimAssaultAlarm(String requestId, Int step, ObjectReference target) Global Native

; Raises Skyrim's normal assault alarm once for a confirmed hostile result on the captured actor, then completes the same open step.
Function AlarmAndComplete(String requestId, Int step, ObjectReference target, String detail) Global
    If !CanExecute(requestId, step)
        Return
    EndIf
    Actor victim = target as Actor
    If victim && !victim.IsDead() && ClaimAssaultAlarm(requestId, step, target)
        victim.SendAssaultAlarm()
        detail += " Skyrim's assault alarm was issued; witness response and any bounty are engine-owned and not verified."
    EndIf
    Complete(requestId, step, "succeeded", detail)
EndFunction

Function Execute(String requestId, Int step, ObjectReference target, String effect, Float value, Bool approved, Scroll sourceSpell = None) Global
    If !CanExecute(requestId, step)
        Return
    EndIf
    If !target || target.IsDeleted() || target.IsDisabled() || !target.Is3DLoaded()
        Complete(requestId, step, "skipped", "The captured target is no longer available.")
        Return
    EndIf
    Actor player = Game.GetPlayer()
    If !player
        Complete(requestId, step, "skipped", "The player is unavailable.")
        Return
    EndIf
    Actor victim = target as Actor
    String status = "unknown"
    String detail = "Operation issued; its physical result is not verified."
    If effect == "pickup"
        Form item = target.GetBaseObject()
        Int countBefore = player.GetItemCount(item)
        If target.IsOffLimits()
            target.SendStealAlarm(player)
        EndIf
        If !CanExecute(requestId, step)
            Return
        EndIf
        player.AddItem(target, 1, True)
        Int pickupChecks = 0
        While CanExecute(requestId, step) && player.GetItemCount(item) == countBefore && pickupChecks < 10
            Utility.Wait(0.1)
            pickupChecks += 1
        EndWhile
        If !CanExecute(requestId, step)
            Return
        EndIf
        If player.GetItemCount(item) == countBefore + 1 && (target.IsDeleted() || target.IsDisabled() || !target.Is3DLoaded())
            status = "succeeded"
            detail = "The actual world item transferred to the player; inventory and reference changes confirmed."
        Else
            detail = "World item transfer requested once; inventory and reference changes did not confirm pickup."
        EndIf
    ElseIf effect == "consume_world"
        Potion food = target.GetBaseObject() as Potion
        If !food || food.IsPoison()
            Complete(requestId, step, "failed", "The target is not edible food or a drinkable potion.")
            Return
        EndIf
        Int heldBefore = player.GetItemCount(food)
        If target.IsOffLimits()
            target.SendStealAlarm(player)
        EndIf
        If !CanExecute(requestId, step)
            Return
        EndIf
        ; Passing the actual reference transfers it; passing its base form would create a copy.
        player.AddItem(target, 1, True)
        If !CanExecute(requestId, step)
            Return
        EndIf
        If player.GetItemCount(food) != heldBefore + 1 || (!target.IsDeleted() && !target.IsDisabled() && target.Is3DLoaded())
            Complete(requestId, step, "unknown", "World item transfer was requested; both inventory and reference changes were not confirmed.")
            Return
        EndIf
        ; Skyrim's standard player equip path consumes food/potions and applies their authored effects.
        player.EquipItem(food, False, True)
        Int consumeChecks = 0
        While CanExecute(requestId, step) && player.GetItemCount(food) == heldBefore + 1 && consumeChecks < 10
            Utility.Wait(0.1)
            consumeChecks += 1
        EndWhile
        If !CanExecute(requestId, step)
            Return
        EndIf
        If player.GetItemCount(food) == heldBefore
            status = "succeeded"
            detail = "World consumable transferred and player consumption confirmed by inventory decrease."
        Else
            detail = "World item transferred to the player, but consumption could not be confirmed."
        EndIf
    ElseIf effect == "injure" && victim && !victim.IsDead()
        Float before = victim.GetActorValue("Health")
        ; Use Skyrim's assault response, without forcing civilians into combat or setting a bounty.
        If !CanExecute(requestId, step)
            Return
        EndIf
        If ClaimAssaultAlarm(requestId, step, target)
            victim.SendAssaultAlarm()
        EndIf
        If !CanExecute(requestId, step)
            Return
        EndIf
        victim.DamageActorValue("Health", value)
        FinishInjury(requestId, step, before)
        Return
    ElseIf effect == "kill" && victim && approved
        ActorBase base = victim.GetActorBase()
        If !base.IsUnique() && (base.IsEssential() || base.IsProtected())
            Complete(requestId, step, "failed", "Shared actor-base protections cannot be overridden safely.")
            Return
        EndIf
        Bool essential = base.IsEssential()
        Bool protectedActor = base.IsProtected()
        base.SetEssential(False)
        base.SetProtected(False)
        victim.Kill(player)
        base.SetEssential(essential)
        base.SetProtected(protectedActor)
        If victim.IsDead()
            status = "succeeded"
            detail = "Target died."
        Else
            status = "failed"
            detail = "The target survived; another protection may apply."
        EndIf
    ElseIf effect == "resize"
        target.SetScale(value)
        If Math.Abs(target.GetScale() - value) < 0.02
            status = "succeeded"
            detail = "Reference scale changed; collision behavior is not established."
        EndIf
    ElseIf effect == "lock" || effect == "unlock"
        If effect == "lock"
            target.SetLockLevel(value as Int)
            target.Lock(True)
            If target.IsLocked() && target.GetLockLevel() == (value as Int)
                status = "succeeded"
            EndIf
        Else
            target.Lock(False)
            If !target.IsLocked()
                status = "succeeded"
            EndIf
        EndIf
        detail = "Lock state checked."
    ElseIf effect == "open" || effect == "close"
        target.SetOpen(effect == "open")
        Int attempts = 0
        While CanExecute(requestId, step) && attempts < 20 && (target.GetOpenState() == 2 || target.GetOpenState() == 4)
            Utility.Wait(0.1)
            attempts += 1
        EndWhile
        If !CanExecute(requestId, step)
            Return
        EndIf
        If (effect == "open" && target.GetOpenState() == 1) || (effect == "close" && target.GetOpenState() == 3)
            status = "succeeded"
        EndIf
        detail = "Opening or closing requested; animated transition may still be running."
    ElseIf effect == "activate"
        If target.Activate(player)
            status = "succeeded"
            detail = "Activation accepted; scripted consequences are not inferred."
        Else
            status = "failed"
            detail = "Default activation was refused or blocked; no pickup is confirmed."
        EndIf
    ElseIf effect == "disable" && approved
        ; Skyrim refuses independent disabling of references controlled by an enable-state parent.
        ; Never disable the parent: that could also remove unrelated linked objects.
        If target.GetEnableParent()
            Complete(requestId, step, "failed", "The captured reference has an enable-state parent; Skyrim refuses to disable it independently.")
            Return
        EndIf
        target.Disable()
        If target.IsDisabled()
            status = "succeeded"
            detail = "Reference disabled; contents and debris were not changed."
        EndIf
    ElseIf effect == "destroy"
        Int oldStage = target.GetCurrentDestructionStage()
        target.DamageObject(value)
        If target.GetCurrentDestructionStage() != oldStage
            status = "succeeded"
            detail = "Authored destruction stage changed."
        EndIf
    ElseIf effect == "push"
        Float oldX = target.GetPositionX()
        Float oldY = target.GetPositionY()
        Float oldZ = target.GetPositionZ()
        If victim
            player.PushActorAway(victim, value)
        Else
            target.ApplyHavokImpulse(0.0, 0.0, 1.0, value)
        EndIf
        Utility.Wait(0.5)
        If !CanExecute(requestId, step)
            Return
        EndIf
        If Math.Abs(target.GetPositionX() - oldX) + Math.Abs(target.GetPositionY() - oldY) + Math.Abs(target.GetPositionZ() - oldZ) > 5.0
            status = "succeeded"
            detail = "Target displacement confirmed after the impulse."
        Else
            detail = "Impulse requested; displacement is not confirmed."
        EndIf
    ElseIf effect == "magic" && sourceSpell && victim
        String statistic = ""
        If Math.Abs(value) == 24
            statistic = "Health"
        ElseIf Math.Abs(value) == 25
            statistic = "Magicka"
        ElseIf Math.Abs(value) == 26
            statistic = "Stamina"
        EndIf
        Float oldValue = 0.0
        If statistic != ""
            oldValue = victim.GetActorValue(statistic)
        EndIf
        Bool hadEffect = victim.HasMagicEffect(sourceSpell.GetNthEffectMagicEffect(0))
        sourceSpell.Cast(player, target)
        Utility.Wait(1.0)
        If !CanExecute(requestId, step)
            Return
        EndIf
        Bool statisticChanged = False
        If statistic != ""
            statisticChanged = (value < 0 && victim.GetActorValue(statistic) < oldValue) || (value > 0 && victim.GetActorValue(statistic) > oldValue)
        EndIf
        If statisticChanged || (!hadEffect && victim.HasMagicEffect(sourceSpell.GetNthEffectMagicEffect(0)))
            status = "succeeded"
            detail = "The selected scroll was consumed and its effect was observed on the target."
        Else
            detail = "Scroll consumed and cast; effect application was not confirmed."
        EndIf
    ElseIf effect == "combat" && victim && !victim.IsDead()
        victim.StartCombat(player)
        Int combatChecks = 0
        While CanExecute(requestId, step) && victim.GetCombatTarget() != player && combatChecks < 10
            Utility.Wait(0.1)
            combatChecks += 1
        EndWhile
        If !CanExecute(requestId, step)
            Return
        EndIf
        If victim.IsInCombat() && victim.GetCombatTarget() == player
            status = "succeeded"
            detail = "Target entered combat with the player."
        Else
            detail = "Combat with the player could not be confirmed."
        EndIf
    Else
        status = "failed"
        detail = "Effect is not applicable."
    EndIf
    If effect == "push" && status == "succeeded" && victim
        AlarmAndComplete(requestId, step, target, detail)
        Return
    EndIf
    Complete(requestId, step, status, detail)
EndFunction

; Poll only the consequence of the already-consumed item, never repeat consumption.
Function VerifyRestoration(String requestId, Int step, Actor target, String actorValue, Float previous) Global
    Int checks = 0
    While CanExecute(requestId, step) && target && target.GetActorValue(actorValue) <= previous && checks < 10
        Utility.Wait(0.1)
        checks += 1
    EndWhile
    If !CanExecute(requestId, step)
        Return
    EndIf
    If target && !target.IsDead() && target.GetActorValue(actorValue) > previous
        Complete(requestId, step, "succeeded", "Item transferred, consumed, and the requested statistic increased.")
    Else
        Complete(requestId, step, "unknown", "Item transferred and consumed; the requested statistic increase was not confirmed.")
    EndIf
EndFunction

; Collect only already-unlocked ranks; do not learn, unlock or equip magic.
Bool Function CanPrepareMagic(String requestId) Global Native
Function AddUnlockedShout(String requestId, Shout selectedShout, Int rank) Global Native
Function RefreshMagicChoices(String requestId) Global Native
Function FinishSelectedMagic(String requestId, Int step) Global Native

Function CollectUnlockedShouts(String requestId, Shout[] shouts) Global
    Int index = 0
    While index < shouts.Length && CanPrepareMagic(requestId)
        Shout selected = shouts[index]
        Int rank = 0
        While rank < 3 && selected.GetNthWordOfPower(rank) && Game.IsWordUnlocked(selected.GetNthWordOfPower(rank))
            rank += 1
        EndWhile
        If rank > 0
            AddUnlockedShout(requestId, selected, rank)
        EndIf
        index += 1
    EndWhile
    RefreshMagicChoices(requestId)
EndFunction

Function CastSelectedMagic(String requestId, Int step, ObjectReference recipient, Spell selected, Shout selectedShout, Int rank) Global
    If !CanExecute(requestId, step) || !recipient || !selected
        Return
    EndIf
    Actor player = Game.GetPlayer()
    If selectedShout
        If rank < 1 || rank > 3 || selectedShout.GetNthSpell(rank - 1) != selected
            Complete(requestId, step, "failed", "Selected shout is no longer known.")
            Return
        EndIf
        Int index = 0
        While index < rank
            If !Game.IsWordUnlocked(selectedShout.GetNthWordOfPower(index))
                Complete(requestId, step, "failed", "Selected shout rank is no longer unlocked.")
                Return
            EndIf
            index += 1
        EndWhile
    ElseIf !player.HasSpell(selected)
        Complete(requestId, step, "failed", "Selected spell is no longer known.")
        Return
    EndIf
    If !CanExecute(requestId, step) || recipient.IsDeleted() || recipient.IsDisabled() || !recipient.Is3DLoaded()
        Return
    EndIf
    ; One authored scripted application, without equipment, resource or cooldown edits.
    selected.Cast(player, recipient)
    Utility.Wait(0.5)
    If CanExecute(requestId, step)
        FinishSelectedMagic(requestId, step)
    EndIf
EndFunction

; Issue one physics impulse on the captured reference, then observe displacement without replaying it.
Function ThrowObject(String requestId, Int step, ObjectReference target, Float x, Float y, Float z, Float magnitude) Global
    If !CanExecute(requestId, step) || !target || target.IsDeleted() || target.IsDisabled() || !target.Is3DLoaded()
        Return
    EndIf
    Float beforeX = target.GetPositionX()
    Float beforeY = target.GetPositionY()
    Float beforeZ = target.GetPositionZ()
    target.ApplyHavokImpulse(x, y, z, magnitude)
    Utility.Wait(0.3)
    If !CanExecute(requestId, step)
        Return
    EndIf
    If target && !target.IsDeleted() && !target.IsDisabled() && target.Is3DLoaded()
        Float dx = target.GetPositionX() - beforeX
        Float dy = target.GetPositionY() - beforeY
        Float dz = target.GetPositionZ() - beforeZ
        If dx * x + dy * y + dz * z > 1.0
            Complete(requestId, step, "succeeded", "Captured object moved in the requested impulse direction; later collision is not predicted.")
            Return
        EndIf
    EndIf
    Complete(requestId, step, "unknown", "One directional impulse was requested; matching displacement was not confirmed.")
EndFunction
