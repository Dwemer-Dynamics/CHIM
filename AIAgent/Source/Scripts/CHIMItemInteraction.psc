Scriptname CHIMItemInteraction Hidden

; Native session checks prevent queued Papyrus work from crossing a load or cancellation.
Bool Function CanExecute(String requestId, Int step) Global Native
Function Complete(String requestId, Int step, String status, String detail) Global Native

Function Execute(String requestId, Int step, ObjectReference target, String effect, Float value, Bool approved, Scroll sourceSpell = None) Global
    If !CanExecute(requestId, step) || !target
        Return
    EndIf
    Actor player = Game.GetPlayer()
    Actor victim = target as Actor
    String status = "unknown"
    String detail = "Operation issued; its physical result is not verified."
    If effect == "injure" && victim && !victim.IsDead()
        Float before = victim.GetActorValue("Health")
        victim.DamageActorValue("Health", value)
        If victim.GetActorValue("Health") < before
            status = "succeeded"
            detail = "Health decreased."
        EndIf
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
            If target.IsLocked()
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
        EndIf
    ElseIf effect == "disable" && approved
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
        Float oldHealth = victim.GetActorValue("Health")
        Bool hadEffect = victim.HasMagicEffect(sourceSpell.GetNthEffectMagicEffect(0))
        sourceSpell.Cast(player, target)
        Utility.Wait(1.0)
        If !CanExecute(requestId, step)
            Return
        EndIf
        If victim.GetActorValue("Health") < oldHealth || (!hadEffect && victim.HasMagicEffect(sourceSpell.GetNthEffectMagicEffect(0)))
            status = "succeeded"
            detail = "The selected scroll was consumed and its effect was observed on the target."
        Else
            detail = "Scroll consumed and cast; effect application was not confirmed."
        EndIf
    ElseIf effect == "combat" && victim && !victim.IsDead()
        victim.StartCombat(player)
        If victim.IsInCombat()
            status = "succeeded"
            detail = "Target entered combat."
        EndIf
    Else
        status = "failed"
        detail = "Effect is not applicable."
    EndIf
    Complete(requestId, step, status, detail)
EndFunction
