Scriptname AIAgentQuestProgressionBridge Hidden
{Static bridge used by the CHIM SKSE plugin to apply server-approved quest actions.}

Function SetQuestStage(int questFormId, int stage) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.SetStage(stage)
    endif
EndFunction

Function SetQuestObjectiveCompleted(int questFormId, int objectiveIndex, bool completed = true) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.SetObjectiveCompleted(objectiveIndex, completed)
    endif
EndFunction

Function SetQuestObjectiveDisplayed(int questFormId, int objectiveIndex, bool displayed = true, bool forceDisplayed = false) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.SetObjectiveDisplayed(objectiveIndex, displayed, forceDisplayed)
    endif
EndFunction

Function SetQuestStageObjective(int questFormId, int stage, int objectiveIndex) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.SetStage(stage)
        Utility.Wait(0.25)
        targetQuest.SetObjectiveDisplayed(objectiveIndex, true, true)
    endif
EndFunction

Function FailAllQuestObjectives(int questFormId) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.FailAllObjectives()
    endif
EndFunction

Function StartQuest(int questFormId) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.Start()
    endif
EndFunction

Function StartQuestStageObjective(int questFormId, int stage, int objectiveIndex) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        if !targetQuest.IsRunning()
            targetQuest.Start()
            Utility.Wait(0.25)
        endif
        targetQuest.SetStage(stage)
        Utility.Wait(0.25)
        targetQuest.SetObjectiveDisplayed(objectiveIndex, true, true)
    endif
EndFunction

Function ExecuteConsoleCommand(String command) Global
    if command != ""
        ConsoleUtil.ExecuteCommand(command)
    endif
EndFunction

Function ExecuteConsoleCommandSequence(String commands) Global
    int splitIndex = StringUtil.Find(commands, "||")
    while splitIndex >= 0
        String command = StringUtil.Substring(commands, 0, splitIndex)
        if command != ""
            ConsoleUtil.ExecuteCommand(command)
            Utility.Wait(0.25)
        endif
        commands = StringUtil.Substring(commands, splitIndex + 2)
        splitIndex = StringUtil.Find(commands, "||")
    endwhile

    if commands != ""
        ConsoleUtil.ExecuteCommand(commands)
    endif
EndFunction

Function StopQuest(int questFormId) Global
    Quest targetQuest = Game.GetForm(questFormId) as Quest
    if targetQuest
        targetQuest.Stop()
    endif
EndFunction

Function StartScene(int sceneFormId) Global
    Scene targetScene = Game.GetForm(sceneFormId) as Scene
    if targetScene
        targetScene.Start()
    endif
EndFunction

Function SetActorValue(int actorFormId, string actorValue, float value) Global
    Actor targetActor = Game.GetForm(actorFormId) as Actor
    if targetActor
        targetActor.SetActorValue(actorValue, value)
    endif
EndFunction

Function SetActorGhost(int actorFormId, bool ghost) Global
    Actor targetActor = Game.GetForm(actorFormId) as Actor
    if targetActor
        targetActor.SetGhost(ghost)
    endif
EndFunction

Function EvaluateActorPackage(int actorFormId) Global
    Actor targetActor = Game.GetForm(actorFormId) as Actor
    if targetActor
        targetActor.EvaluatePackage()
    endif
EndFunction

Function RemoveItemFromPlayer(int itemFormId, int count = 1, bool silent = false) Global
    Form targetItem = Game.GetForm(itemFormId)
    Actor player = Game.GetPlayer()
    if targetItem && player
        player.RemoveItem(targetItem, count, silent)
    endif
EndFunction

Function AddItemToPlayer(int itemFormId, int count = 1, bool silent = false) Global
    Form targetItem = Game.GetForm(itemFormId)
    Actor player = Game.GetPlayer()
    if targetItem && player
        player.AddItem(targetItem, count, silent)
    endif
EndFunction

Function EnableReference(int refFormId, bool fadeIn = false) Global
    ObjectReference targetRef = Game.GetForm(refFormId) as ObjectReference
    if targetRef
        targetRef.Enable(fadeIn)
    endif
EndFunction

Function SetActorRelationshipToPlayer(int actorFormId, int rank) Global
    Actor targetActor = Game.GetForm(actorFormId) as Actor
    Actor player = Game.GetPlayer()
    if targetActor && player
        targetActor.SetRelationshipRank(player, rank)
    endif
EndFunction
