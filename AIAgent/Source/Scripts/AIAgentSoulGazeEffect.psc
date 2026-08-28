Scriptname AIAgentSoulGazeEffect 


function Soulgaze(int mode) global
	
	String hints="";
	
	int customMode=mode
	
	
	if ((AIAgentFunctions.isGameVR()==1) || customMode==0)
		
		Consoleutil.ExecuteCommand("tm");
		Utility.wait(1);
		AIAgentFunctions.shotAndUpload(hints,0)
		Consoleutil.ExecuteCommand("tm");
		
	else
		
		;Game.ForceFirstPerson();
		
		
		; Try to guess wich actors are in camera
		Actor[] actors = MiscUtil.ScanCellNPCs(Game.GetPlayer())
		; remove player actor from the list
		actors = PapyrusUtil.RemoveActor(actors,Game.GetPlayer())
		int i = actors.length - 1
		Actor actorAtIndex = None

		; iterating reversed as we modify the array
		while i>=0
			actorAtIndex = actors[i]
			if (Game.GetPlayer().HasLOS(actorAtIndex))
				hints=hints+actorAtIndex.GetDisplayName()+",";
			endif
			i -= 1
		endwhile
		
		Consoleutil.ExecuteCommand("tm");
		AIAgentFunctions.shotAndUpload(hints,1)
		Utility.wait(1);
		Consoleutil.ExecuteCommand("tm");
	endif 
endFunction

function SendProfilePicture(int mode,bool zoom) global
	
	String hints="";
	
	int customMode=mode
	
	
	if ((AIAgentFunctions.isGameVR()==1) || customMode==0)
		
		Consoleutil.ExecuteCommand("tm");
		if (zoom)
			Consoleutil.ExecuteCommand("fov 60");
			Consoleutil.ExecuteCommand("tfc");
		endif
		Utility.wait(1);
		AIAgentFunctions.shotAndUpload(hints,2)
		Utility.wait(1);
		if (zoom)
			Consoleutil.ExecuteCommand("tfc");
		endif
		Consoleutil.ExecuteCommand("tm");
		
	else
		
		;Game.ForceFirstPerson();
		
		
		; Try to guess wich actors are in camera
		Actor[] actors = MiscUtil.ScanCellNPCs(Game.GetPlayer())
		; remove player actor from the list
		actors = PapyrusUtil.RemoveActor(actors,Game.GetPlayer())
		int i = actors.length - 1
		Actor actorAtIndex = None

		; iterating reversed as we modify the array
		while i>=0
			actorAtIndex = actors[i]
			if (Game.GetPlayer().HasLOS(actorAtIndex))
				hints=hints+actorAtIndex.GetDisplayName()+",";
			endif
			i -= 1
		endwhile
		
		Consoleutil.ExecuteCommand("tm");
		if (zoom)
			Consoleutil.ExecuteCommand("fov 60");
			Consoleutil.ExecuteCommand("tfc");
		endif
		
		Utility.wait(1);
		AIAgentFunctions.shotAndUpload(hints,3)
		Utility.wait(1);
		if (zoom)
			Consoleutil.ExecuteCommand("tfc");
		endif
		Consoleutil.ExecuteCommand("tm");
		
	endif 
endFunction


function JustUpload(int mode) global
	
	String hints="";
	
	int customMode=mode
	
	
	if ((AIAgentFunctions.isGameVR()==1) || customMode==0)
		
		Consoleutil.ExecuteCommand("tm");
		Utility.wait(1);
		AIAgentFunctions.shotAndUpload(hints,4)
		Consoleutil.ExecuteCommand("tm");
		
	else
		
		;Game.ForceFirstPerson();
		
		
		; Try to guess wich actors are in camera
		Actor[] actors = MiscUtil.ScanCellNPCs(Game.GetPlayer())
		; remove player actor from the list
		actors = PapyrusUtil.RemoveActor(actors,Game.GetPlayer())
		int i = actors.length - 1
		Actor actorAtIndex = None

		; iterating reversed as we modify the array
		while i>=0
			actorAtIndex = actors[i]
			if (Game.GetPlayer().HasLOS(actorAtIndex))
				hints=hints+actorAtIndex.GetDisplayName()+",";
			endif
			i -= 1
		endwhile
		
		Consoleutil.ExecuteCommand("tm");
		AIAgentFunctions.shotAndUpload(hints,5)
		Utility.wait(1);
		Consoleutil.ExecuteCommand("tm");
	endif 
endFunction

; Run the gesture-driven capture without opening the deprecated Soulgaze wheel.
int function StartGestureCapture(int mode, int captureType, Actor target = None, bool zoom = false) global
	Consoleutil.ExecuteCommand("tm")
	if (zoom)
		Consoleutil.ExecuteCommand("fov 60")
		Consoleutil.ExecuteCommand("tfc")
	endif

	Utility.Wait(1.0)
	int result = AIAgentFunctions.startSoulgazeCapture("", captureType, mode, target)
	Utility.Wait(1.0)

	if (zoom)
		Consoleutil.ExecuteCommand("tfc")
	endif
	Consoleutil.ExecuteCommand("tm")

	if (result == -1)
		Debug.Trace("[CHIM] Soulgaze capture rejected because another capture is active")
		Debug.Notification("[CHIM] Soulgaze is already processing a capture.")
	elseif (result == 0)
		Debug.Trace("[CHIM] Soulgaze capture could not be started")
		Debug.Notification("[CHIM] Soulgaze capture failed. Check AIAgent.log.")
	endif
	return result
endFunction

int function CaptureContext(int mode) global
	return StartGestureCapture(mode, 0)
endFunction

int function CapturePortrait(int mode, Actor target) global
	return StartGestureCapture(mode, 1, target, false)
endFunction

int function DescribeScene(int mode, Actor target) global
	return StartGestureCapture(mode, 2, target)
endFunction
