Scriptname AIAgentMCMConfigScript extends SKI_ConfigBase  


AIAgentPapyrusFunctions Property controlScript Auto

int			_keymapOID_K
int			_myKey					= -1

int			_keymapOID_K2
int			_myKey2					= -1

int			_keymapOID_K3
int			_myKey3					= -1

int			_keymapOID_K4
int			_myKey4					= -1

int			_toggle1OID_B
bool		_toggleState1			= true

int			_toggle1OID_C
bool		_toggleState2			= true

int			_toggle1OID_D
bool		_toggleState3			= false

int			_keymapOID_K5
int			_myKey5				= -1

int			_keymapOID_K6
int			_myKey6				= -1

int			_keymapOID_K7
int			_myKey7				= -1


int			_slider_volume
float		_sound_volume				= 50.0

int			_slider_head_voice_volume
float		_head_voice_volume			= 100.0

int			_slider_preclip
float		_sound_preclip				= 100.0

int			_slider_postclip
float		_sound_postclip				= 100.0

int			_slider_ds
float		_sound_ds				= 2.0

int			_slider_playback_dropoff_inside
float		_playback_dropoff_inside		= 70.0

int			_slider_playback_dropoff_outside
float		_playback_dropoff_outside		= 70.0


int			_toggle1OID_E
bool		_toggleState7			= true


int			_slider_lip_res
float		_lip_res				= 500.0

int			_slider_lip_int
float		_lip_int				= 1.0


int 		_slider_timeout
float		_timeout_int				= 60.0


int			_toggleAnimation
bool		_animationstate			= false


int			_toggle1OID_Rereg

; Saved native setting retained for compatibility with the Audio Mode presets.
bool		_enable3daudioplaybackstate		= true

int			_toggleCameraBasedAudio
bool		_camera_based_audio_state		= false

int			_toggleInvertHeading
bool		_invertheadingstate			= false

int			_togglePauseDialogue
bool		_pauseDialogueState			= false

int			_togglePlayerTtsTraditionalDialogue
bool		_playerTtsTraditionalDialogueState	= false

int			_toggleCaptureBackgroundChat
bool		_captureBackgroundChatState		= true


; Auto Activate related

int			_toggleAddAllNPC 
bool		_toggleAddAllNPCState			= false

int			_slider_max_distance_inside
float		_max_distance_inside		= 1200.0

int			_slider_max_distance_outside
float		_max_distance_outside		= 2400.0

int			_slider_spatial_hearing_inside
float		_spatial_hearing_inside		= 500.0

int			_slider_spatial_hearing_outside
float		_spatial_hearing_outside	= 1000.0

int			_slider_auto_hearing_radius_m
float		_auto_hearing_radius_m	= 8.0

int			_slider_bored_period
float		_bored_period		= 60.0

int			_slider_dynamic_profile_period
float		_dynamic_profile_period		= 20.0

int 		_toggleRechat_policy_asap
bool  		_rechat_policy_asap = true

int 		_toggle_npc_go_near
bool  		_toggle_npc_go_near_state = true

int 		_toggle_npc_walk_to_target
bool  		_toggle_npc_walk_to_target_state = false

int 		_toggle_autofocus_on_sit
bool  		_toggle_autofocus_on_sit_state = false

int 		_toggle_usewebsocketstt
bool  		_toggle_usewebsocketstt_state = false


int 		_toggle_restrict_onscene
bool  		_toggle_restrict_onscene_state = false


int 		_toggle_autoadd_hostile
bool  		_toggle_autoadd_hostile_state = false


int 		_toggle_autoadd_allraces
bool  		_toggle_autoadd_allraces_state = false


int 		_toggle_autoadd_creature_npcs
bool  		_toggle_autoadd_creature_npcs_state = false


; Open Mic functionality
int			_toggle_openmic
bool		_toggle_openmic_state			= false

int			_slider_openmic_sensitivity
float		_openmic_sensitivity			= 1000.0

int			_slider_openmic_enddelay
float		_openmic_enddelay				= 1.0

int			_keymap_openmic_mute
int			_openmic_mute_key				= -1

int			_text_current_recording_device


int			_keymap_godmode
int			_godmode_key					= -1

int			_keymap_halt
int			_halt_key					= -1

int			_keymap_masterwheel
int			_masterwheel_key				= -1

; CHIM Browser Beta (Prisma UI)
int			_keymap_browser
int			_browser_key					= -1

; CHIM Debugger (Prisma UI)
int			_keymap_debugger
int			_debugger_key					= -1

; CHIM Chatbox (Prisma UI)
int			_keymap_chatbox
int			_chatbox_key					= -1

; CHIM Chatbox Type Message
int			_keymap_chatbox_focus
int			_chatbox_focus_key				= -1

; CHIM Settings Menu (Prisma UI)
int			_keymap_settingsmenu
int			_settingsmenu_key				= -1

; CHIM Master Menu (Prisma UI)
int			_keymap_mastermenu
int			_mastermenu_key					= -1

; Gesture-driven Soulgaze capture
int			_keymap_soulgaze
int			_soulgaze_key					= -1

; CHIM Overlay/Status Cycle (Single hotkey)
int			_keymap_overlaystatus_cycle
int			_overlaystatus_cycle_key		= -1

; CHIM History/Diaries Cycle (Single hotkey)
int			_keymap_historydiaries_cycle
int			_historydiaries_cycle_key		= -1

int			_actionSendLocations
bool		_actionSendLocationsState		= false
int			_actionSendVoices


; combat dialogue
int			_toggle_combatdialogue
bool		_toggle_combatdialogue_state		= true

; Cancel dialogue on combat entry
int			_toggle_cancel_dialogue_on_combat
bool		_toggle_cancel_dialogue_on_combat_state		= true

; Combat barks
int			_toggle_combat_barks
bool		_toggle_combat_barks_state		= true

int			_slider_combat_barks_period
float		_combat_barks_period			= 30.0


; AI Agents page variables
int[] 		_agentToggleOIDs
string[]	_currentAgentNames
int			_toggleAddAllNowNPC
int			_removeAllAgentsOID

; New variables for nearby non-agent NPCs
int[]		_nearbyNpcToggleOIDs
string[]	_nearbyNpcNames
int			_refreshNearbyNPCsOID



; default settings
int			_myKeyDefault					= -1
int			_myKey2Default					= -1
int			_myKey3Default					= -1
int			_myKey4Default					= -1
int			_myKey5Default					= -1
int			_myKey6Default					= -1
int			_myKey7Default					= -1
bool		_toggleState2Default			= false
float		_sound_volumeDefault			= 75.0
float		_head_voice_volumeDefault		= 100.0
float		_sound_preclipDefault			= 100.0
float		_sound_postclipDefault			= 0.0
float		_sound_dsDefault				= 2.0
float		_playback_dropoff_insideDefault		= 70.0
float		_playback_dropoff_outsideDefault	= 70.0
bool		_toggleState7Default			= true
float		_lip_resDefault					= 500.0
float		_lip_intDefault					= 1.0
float		_timeout_intDefault				= 30.0
bool		_animationstateDefault			= false
bool		_enable3daudioplaybackstateDefault	= true
bool		_camera_based_audio_stateDefault	= false
bool		_invertheadingstateDefault		= false
bool		_pauseDialogueStateDefault		= false
bool		_playerTtsTraditionalDialogueStateDefault = false
bool		_captureBackgroundChatStateDefault	= true
bool 		_rechat_policy_asap_default		= true
bool		_toggle_openmic_stateDefault	= false
float		_openmic_sensitivityDefault		= 1000.0
float		_openmic_enddelayDefault		= 1.0
int			_openmic_mute_keyDefault		= -1
int			_soulgaze_keyDefault			= -1
bool		_toggle_cancel_dialogue_on_combat_stateDefault = true
bool		_toggle_combat_barks_stateDefault	= true
float		_combat_barks_periodDefault		= 30.0

int			_halt_keyDefault				= -1

int			_masterwheel_keyDefault			= -1

int			_overlaystatus_cycle_keyDefault	= -1

int			_historydiaries_cycle_keyDefault = -1

int _prismaMcmRevision = 0

int _slider_curve_legacy_distance 
float _curve_legacy_distance = 1.0

int _slider_maintenance_period
float _maintenance_period = 4.0

; Retired toggle, kept so the one-time Audio Mode migration can still read it out of old saves.
bool		_toggle_force_mono_state		= false

; Audio Mode selects the existing advanced, mono and legacy attenuation settings.
; Keep the earlier draft's IDs for save migration: 0 Flat, 1 Legacy, 2 Advanced.
int _menu_audio_mode
int _audio_mode = -1
int _audio_modeDefault = 2
string[] _audio_mode_options
bool _audio_modes_v2 = false
float _legacy_distance_saved = 1.0

; Rebuild labels for saves made before the five-choice dropdown existed.
string[] Function AudioModeOptions()
	if (!_audio_mode_options || _audio_mode_options.Length != 5)
		_audio_mode_options = new string[5]
	endIf
	_audio_mode_options[0] = "3D Advanced"
	_audio_mode_options[1] = "3D Legacy"
	_audio_mode_options[2] = "2D Flat"
	_audio_mode_options[3] = "Mono"
	_audio_mode_options[4] = "Mono + Advanced Effects"
	return _audio_mode_options
EndFunction

; Convert between menu order and saved mode IDs; swapping 0 and 2 works in both directions.
int Function AudioModeMenuIndex(int mode)
	if (mode == 0 || mode == 2)
		return 2 - mode
	endIf
	return mode
EndFunction

string Function AudioModeLabel(int mode)
	string[] modes = AudioModeOptions()
	if (mode >= 0 && mode < modes.Length)
		return modes[AudioModeMenuIndex(mode)]
	endIf
	return modes[0]
EndFunction

; Distinguish original saves from the unmerged three-mode draft, whose Flat mode was mono.
Function MigrateAudioMode()
	if (_audio_modes_v2)
		return
	endIf
	if (_audio_mode == 0)
		_audio_mode = 3
	elseif (_audio_mode == 1)
		if ((_curve_legacy_distance as Int) < 1)
			_audio_mode = 0
		endIf
	elseif (_audio_mode != 2)
		if (_toggle_force_mono_state)
			_audio_mode = 3
			if (_enable3daudioplaybackstate)
				_audio_mode = 4
			endIf
		elseif (_enable3daudioplaybackstate)
			_audio_mode = 2
		elseif ((_curve_legacy_distance as Int) < 1)
			_audio_mode = 0
		else
			_audio_mode = 1
		endIf
	endIf
	if ((_curve_legacy_distance as Int) >= 1)
		_legacy_distance_saved = _curve_legacy_distance
	endIf
	_audio_modes_v2 = true
EndFunction

; Apply presets through the original native keys, retaining dormant distance tuning.
Function ApplyAudioMode(bool restoreDistance = false)
	MigrateAudioMode()
	_enable3daudioplaybackstate = _audio_mode == 2 || _audio_mode == 4
	_toggle_force_mono_state = _audio_mode == 3 || _audio_mode == 4
	if (_audio_mode == 0)
		if ((_curve_legacy_distance as Int) >= 1)
			_legacy_distance_saved = _curve_legacy_distance
		endIf
		_curve_legacy_distance = 0.0
	elseif (_audio_mode == 1 && (_curve_legacy_distance as Int) < 1)
		_curve_legacy_distance = _legacy_distance_saved
	endIf
	; The engine shares its distance scaler: avoid rewriting it when only toggling mono/advanced.
	if (restoreDistance || _audio_mode == 0 || _audio_mode == 1)
		controlScript.setConf("_curve_legacy_distance", _curve_legacy_distance)
	endIf
	controlScript.setConf("_force_mono", _toggle_force_mono_state as Int)
	controlScript.setConf("_enable_3d_audio_playback", _enable3daudioplaybackstate as Int)
EndFunction

; All directional modes use the existing camera and inverted heading controls.
bool Function IsAudioMode3D()
	return _audio_mode == 0 || _audio_mode == 1 || _audio_mode == 2
EndFunction

int Function AudioModeOptionFlags(bool editable)
	if (editable)
		return OPTION_FLAG_NONE
	endIf
	return OPTION_FLAG_DISABLED
EndFunction

event OnPlayerLoadGame()
	; The quest's pending key state is saved; do not replay a gesture from the loaded save.
	if (controlScript)
		controlScript.ResetChatHotkeys()
	endif
	RegisterPrismaMCMEvent()
	; Re-apply combat settings on every game load since C++ plugin doesn't persist them
	Debug.Trace("[CHIM] OnPlayerLoadGame")
	int combatDialogueValue = AIAgentFunctions.get_conf_i("_combat_dialogue")
	if (combatDialogueValue > 0)
		_toggle_combatdialogue_state = true
		controlScript.setConf("_combat_dialogue", 1)
	else
		_toggle_combatdialogue_state = false
		controlScript.setConf("_combat_dialogue", 0)
	endIf
	
	int combatBarksValue = AIAgentFunctions.get_conf_i("_combat_barks")
	if (combatBarksValue > 0)
		_toggle_combat_barks_state = true
		controlScript.setConf("_combat_barks", 1)
	else
		_toggle_combat_barks_state = false
		controlScript.setConf("_combat_barks", 0)
	endIf
	
	int combatBarksPeriodValue = AIAgentFunctions.get_conf_i("_combat_barks_period")
	if (combatBarksPeriodValue >= 5)
		_combat_barks_period = combatBarksPeriodValue as float
		controlScript.setConf("_combat_barks_period", _combat_barks_period)
	else
		_combat_barks_period = 30.0
		controlScript.setConf("_combat_barks_period", 30.0)
	endIf
	
	int cancelDialogueValue = AIAgentFunctions.get_conf_i("_cancel_dialogue_on_combat")
	if (cancelDialogueValue > 0)
		_toggle_cancel_dialogue_on_combat_state = true
		controlScript.setConf("_cancel_dialogue_on_combat", 1)
	else
		_toggle_cancel_dialogue_on_combat_state = false
		controlScript.setConf("_cancel_dialogue_on_combat", 0)
	endIf

	; Restore Player TTS from the saved MCM state instead of the DLL's current runtime flag.
	; The native flag boots false on a fresh load, so reading it here forces the feature off.
	if (_playerTtsTraditionalDialogueState)
		controlScript.setConf("_player_tts_traditional_dialogue", 1)
	else
		controlScript.setConf("_player_tts_traditional_dialogue", 0)
	endIf

	if (_captureBackgroundChatState)
		controlScript.setConf("_capture_background_chat", 1)
	else
		controlScript.setConf("_capture_background_chat", 0)
	endIf

	; Restore the saved Audio Mode; the native flag boots to its own default on a fresh load.
	ApplyAudioMode(true)

	if (_camera_based_audio_state)
		controlScript.setConf("_camera_based_audio", 1)
	else
		controlScript.setConf("_camera_based_audio", 0)
	endIf

	if (_camera_based_audio_state)
		controlScript.setConf("_camera_based_audio", 1)
	else
		controlScript.setConf("_camera_based_audio", 0)
	endIf
	

endEvent

event OnConfigInit()

	; Resolve saved legacy flags before any initialization defaults can replace them.
	MigrateAudioMode()
	ModName="CHIM"
	RegisterPrismaMCMEvent()
	Pages = new string[6]
	Pages[0] = "Hotkeys"
	Pages[1] = "Auto Activate"
	Pages[2] = "Behavior"
	Pages[3] = "Sound"
	Pages[4] = "AI Agents"
	Pages[5] = "Tools"
	
	Debug.Trace("[AIAGENT] OnConfigInit");
	
	;_sound_postclip				= 0.0
	;_sound_preclip				= 100.0
	;_sound_volume				= 75 
	;_head_voice_volume			= 100
	;_lip_res				= 500.0
	;_lip_int				= 1.0
	
	if (CurrentVersion>1)
		_sound_ds					= 2.0
	endIf
	if (CurrentVersion<25)
		_toggleState7= true
		_toggle1OID_E = 1
	endIf
	if (CurrentVersion<28)
		_animationstate= false
		_toggleAnimation = 1
	endIf
	if (CurrentVersion<26)
		;controlScript.setSoulgazeModeNative(1)
	endIf
	if (CurrentVersion<29)
		SetTitleText("CHIM")
	endIf
	
	if (CurrentVersion<35)
		controlScript.setConf("_rechat_policy_asap",0); Smart rechat is always on
		_rechat_policy_asap=true
	endIf
	
	if (CurrentVersion<37)
		_bored_period=60
		StorageUtil.SetIntValue(None, "AIAgentNpcWalkNear",1);
		_toggle_npc_go_near_state=true
	endIf
	
	if (CurrentVersion<43)
		_dynamic_profile_period=20
	endIf

	if (CurrentVersion<66)
		_camera_based_audio_state = false
	endIf

	if (CurrentVersion<73)
		_captureBackgroundChatState = true
	endIf
	
	; Load combat dialogue settings
	int combatDialogueValue = AIAgentFunctions.get_conf_i("_combat_dialogue")
	if (combatDialogueValue > 0)
		_toggle_combatdialogue_state = true
		controlScript.setConf("_combat_dialogue", 1)
	else
		_toggle_combatdialogue_state = false
		controlScript.setConf("_combat_dialogue", 0)
	endIf
	
	; Load combat barks settings
	int combatBarksValue = AIAgentFunctions.get_conf_i("_combat_barks")
	if (combatBarksValue > 0)
		_toggle_combat_barks_state = true
		controlScript.setConf("_combat_barks", 1)
	else
		_toggle_combat_barks_state = false
		controlScript.setConf("_combat_barks", 0)
	endIf
	
	int combatBarksPeriodValue = AIAgentFunctions.get_conf_i("_combat_barks_period")
	if (combatBarksPeriodValue >= 5)
		_combat_barks_period = combatBarksPeriodValue as float
		controlScript.setConf("_combat_barks_period", _combat_barks_period)
	else
		_combat_barks_period = 30.0
		controlScript.setConf("_combat_barks_period", 30.0)
	endIf
	
	; Load cancel dialogue on combat setting
	int cancelDialogueValue = AIAgentFunctions.get_conf_i("_cancel_dialogue_on_combat")
	if (cancelDialogueValue > 0)
		_toggle_cancel_dialogue_on_combat_state = true
		controlScript.setConf("_cancel_dialogue_on_combat", 1)
	else
		_toggle_cancel_dialogue_on_combat_state = false
		controlScript.setConf("_cancel_dialogue_on_combat", 0)
	endIf

	int traditionalDialoguePlayerTtsValue = AIAgentFunctions.get_conf_i("_player_tts_traditional_dialogue")
	if (traditionalDialoguePlayerTtsValue > 0)
		_playerTtsTraditionalDialogueState = true
		controlScript.setConf("_player_tts_traditional_dialogue", 1)
	else
		_playerTtsTraditionalDialogueState = false
		controlScript.setConf("_player_tts_traditional_dialogue", 0)
	endIf

	if (_captureBackgroundChatState)
		controlScript.setConf("_capture_background_chat", 1)
	else
		controlScript.setConf("_capture_background_chat", 0)
	endIf

	; Load spatial hearing distance settings
	int spatialHearingInsideValue = AIAgentFunctions.get_conf_i("_spatial_hearing_inside")
	if (spatialHearingInsideValue > 0)
		_spatial_hearing_inside = spatialHearingInsideValue as float
	else
		_spatial_hearing_inside = 500.0
	endIf
	controlScript.setConf("_spatial_hearing_inside", _spatial_hearing_inside)

	int spatialHearingOutsideValue = AIAgentFunctions.get_conf_i("_spatial_hearing_outside")
	if (spatialHearingOutsideValue > 0)
		_spatial_hearing_outside = spatialHearingOutsideValue as float
	else
		_spatial_hearing_outside = 1000.0
	endIf
	controlScript.setConf("_spatial_hearing_outside", _spatial_hearing_outside)

	int autoHearingRadiusValue = AIAgentFunctions.get_conf_i("_auto_hearing_radius_m")
	if (autoHearingRadiusValue < 1 || autoHearingRadiusValue > 20)
		autoHearingRadiusValue = AIAgentFunctions.get_conf_i("_player_auto_include_radius_m")
	endIf
	if (autoHearingRadiusValue >= 1 && autoHearingRadiusValue <= 20)
		_auto_hearing_radius_m = autoHearingRadiusValue as float
	else
		_auto_hearing_radius_m = 8.0
	endIf
	controlScript.setConf("_auto_hearing_radius_m", _auto_hearing_radius_m)

	int playbackDropoffInsideValue = AIAgentFunctions.get_conf_i("_playback_dropoff_inside")
	if (playbackDropoffInsideValue >= 25 && playbackDropoffInsideValue <= 200)
		_playback_dropoff_inside = playbackDropoffInsideValue as float
	else
		_playback_dropoff_inside = 70.0
	endIf
	controlScript.setConf("_playback_dropoff_inside", _playback_dropoff_inside)

	int playbackDropoffOutsideValue = AIAgentFunctions.get_conf_i("_playback_dropoff_outside")
	if (playbackDropoffOutsideValue >= 25 && playbackDropoffOutsideValue <= 200)
		_playback_dropoff_outside = playbackDropoffOutsideValue as float
	else
		_playback_dropoff_outside = 70.0
	endIf
	controlScript.setConf("_playback_dropoff_outside", _playback_dropoff_outside)

	ApplyAudioMode(true)

	if (_camera_based_audio_state)
		controlScript.setConf("_camera_based_audio", 1)
	else
		controlScript.setConf("_camera_based_audio", 0)
	endIf
	
	if (CurrentVersion<38)
		_toggle_autofocus_on_sit=0
		StorageUtil.SetIntValue(None, "AIAgentAutoFocusOnSit",0);
		_toggle_autofocus_on_sit_state=false
	endIf
	
	if (CurrentVersion<39)
		_toggle_usewebsocketstt=0 
		StorageUtil.SetIntValue(None, "AIAgentWebSockeSTT",0);
		_toggle_usewebsocketstt_state=false
		

		controlScript.setConf("_restrict_onscene",1)
		_toggle_restrict_onscene=1 
		_toggle_restrict_onscene_state=true;
	endIf
	
	if (CurrentVersion<41)
		controlScript._currentGodmodeStatus=false
		controlScript.setConf("_godmode",0)
		controlScript.setConf("_autoadd_hostile",0)
		controlScript.setConf("_autoadd_allraces",0)
	endIf

	
	ConsoleUtil.ExecuteCommand("setstage SKI_ConfigManagerInstance 1")
endEvent

int function GetVersion()

	return 76

endFunction

event OnVersionUpdate(int a_version)
	; a_version is the new version, CurrentVersion is the old version

	if (a_version == 76 && a_version > CurrentVersion)
		; Version 76: Present the original five audio combinations without resetting settings.
		ApplyAudioMode(true)
		RegisterPrismaMCMEvent()
		_prismaMcmRevision += 1
		PublishPrismaMCMState()
		if (UI.IsMenuOpen("Journal Menu"))
			ForcePageReset()
		endIf
		return
	endIf

	if (a_version == 74 && a_version > CurrentVersion)
		OnConfigInit()
	endif
	
	if (a_version == 73 && a_version > CurrentVersion)
		OnConfigInit()
	endif

	if (a_version == 72 && a_version > CurrentVersion)
		OnConfigInit()
	endif
	
	if (a_version == 71 && a_version > CurrentVersion)
		; Version 71: Refresh the SoulGaze hotkey entry for saves already on 70. Keeps every
		; stored setting, including _soulgaze_key, so OnConfigInit is deliberately not called.
		if (CurrentVersion < 70)
			; Saves that never reached 70 still need the original SoulGaze setup and the
			; OnConfigInit catch-up the version 70 block used to give them.
			_soulgaze_key = -1
			OnConfigInit()
		endIf
		RegisterPrismaMCMEvent()
		if (_soulgaze_key != -1)
			controlScript.doBinding20(_soulgaze_key)
		endIf
		_prismaMcmRevision += 1
		PublishPrismaMCMState()
		if (UI.IsMenuOpen("Journal Menu"))
			ForcePageReset()
		endIf
	endIf

	if (a_version == 70 && a_version > CurrentVersion)
		; Version 70: Added the independent gesture-driven Soulgaze hotkey.
		_soulgaze_key = -1
		OnConfigInit()
	endIf

	if (a_version == 69 && a_version > CurrentVersion)
		OnConfigInit()
	endIf
	
	if (a_version == 69 && a_version > CurrentVersion)
		; Version 69: Added independent narrator and player TTS playback volume
		_head_voice_volume = 100.0
		controlScript.setConf("_head_voice_volume", _head_voice_volume)
		OnConfigInit()
	endIf

	if (a_version == 68 && a_version > CurrentVersion)
		; Version 68: Reworked MCM hotkeys and moved behavior settings
		OnConfigInit()
	endIf

	if (a_version == 67 && a_version > CurrentVersion)
		; Version 67: Added auto hearing radius slider
		OnConfigInit()
	endIf

	if (a_version == 66 && a_version > CurrentVersion)
		; Version 66: Add camera-based audio heading option
		OnConfigInit()
	endIf

	if (a_version == 65 && a_version > CurrentVersion)
		; Version 65: Refresh Prisma CHIM Chat / Chatbox View labels and hotkey menu state
		OnConfigInit()
	endIf

	if (a_version == 64 && a_version > CurrentVersion)
		; Version 64: Reordered Prisma UI hotkeys and restored Dialogue History entry
		OnConfigInit()
	endIf

	if (a_version == 63 && a_version > CurrentVersion)
		; Version 63: Simplified Prisma UI hotkeys and updated Prisma labels
		OnConfigInit()
	endIf

	if (a_version == 62 && a_version > CurrentVersion)
		; Version 62: Moved Recording Device section below Open Mic Settings
		OnConfigInit()
	endIf

	if (a_version == 61 && a_version > CurrentVersion)
		; Version 61: Renamed Recording Device row to Current Device for clearer MCM display
		OnConfigInit()
	endIf

	if (a_version == 60 && a_version > CurrentVersion)
		; Version 60: Added dedicated Recording Device section to the Sound page
		OnConfigInit()
	endIf

	if (a_version == 59 && a_version > CurrentVersion)
		; Version 59: Added player-facing 3D audio playback toggle
		OnConfigInit()
	endIf

	if (a_version == 58 && a_version > CurrentVersion)
		; Version 58: Added optional Player TTS for traditional dialogue toggle
		OnConfigInit()
	endIf

	if (a_version == 57 && a_version > CurrentVersion)
		; Version 57: Restored CHIM MCM name and clarified Quest Creator entry point
		OnConfigInit()
	endIf

	if (a_version == 56 && a_version > CurrentVersion)
		; Version 56: Added playback dropoff aggressiveness sliders for indoor/outdoor audio playback
		OnConfigInit()
	endIf

	if (a_version == 55 && a_version > CurrentVersion)
		; Version 55: Added spatial awareness hearing range sliders
		OnConfigInit()
	endIf

	if (a_version == 54 && a_version > CurrentVersion)
		; Version 54: Added CHIM Master Menu to Prisma UI page
		OnConfigInit()
	endIf

	if (a_version == 53 && a_version > CurrentVersion)
		; Version 53: Added new Prisma UI page to MCM menu
		OnConfigInit()
	endIf

	if (a_version == 52 && a_version > CurrentVersion)
		; Version 52: Added Chatbox Type Message keybinding
		_chatbox_focus_key = -1
		OnConfigInit()
	endIf

	if (a_version == 51 && a_version > CurrentVersion)
		; Version 51: Added CHIM Chatbox keybinding
		_chatbox_key = -1
		OnConfigInit()
	endIf

	if (a_version == 50 && a_version > CurrentVersion)
		OnConfigInit()
		
	endIf

	if (a_version == 49 && a_version > CurrentVersion)
		OnConfigInit()
		
	endIf

	if (a_version == 48 && a_version > CurrentVersion)
		OnConfigInit()
		
	endIf
	
	if (a_version == 47 && CurrentVersion < 45)
		OnConfigInit()
		
	endIf
	
	if (a_version >= 2 && CurrentVersion < 45)
		OnConfigInit()
		
		; Clear any AutoActivate related settings from existing saves
		if (CurrentVersion < 37)
			controlScript.setConf("_max_distance_inside", 0.0)
			controlScript.setConf("_max_distance_outside", 0.0)
			controlScript.setConf("_bored_period", 60)
			controlScript.setConf("_toggleAddAllNPC", 0)
		endif
		
	endIf
	
	if (CurrentVersion==0)
		Debug.Trace("First install detected")
		controlScript.setConf("_toggleAddAllNPC", 1)
		_toggleAddAllNPC=1
		_toggleAddAllNPCState=true
		
	endif;
endEvent


int function getActionMode() 

	if (_toggleState2)
		controlScript.setNewActionMode(1)
	else
		controlScript.setNewActionMode(0)
		
	endif
	return 0
EndFunction

Function RegisterPrismaMCMEvent()
	UnregisterForModEvent("CHIM_PrismaMCMRequest")
	RegisterForModEvent("CHIM_PrismaMCMRequest", "OnPrismaMCMRequest")
EndFunction

String Function PrismaMCMBool(bool value)
	if value
		return "1"
	endif
	return "0"
EndFunction

; Prisma's readonly metadata flag: controls that the current Audio Mode does not use stay visible
; but greyed out, so their saved values survive a round trip through another mode.
String Function PrismaMCMReadonly(bool editable)
	if editable
		return "0"
	endif
	return "1"
EndFunction

Function PublishPrismaMCMEntry(String pageName, String sectionName, String keyName, String label, String description, String controlType, String value, String options)
	AIAgentFunctions.publishChimMcmEntry(pageName, sectionName, keyName, label, description, controlType, value, options)
EndFunction

Function PublishPrismaMCMState()
	; Resolve the sentinel first: the Sound rows publish their readonly flags from the mode.
	MigrateAudioMode()
	AIAgentFunctions.beginChimMcmSnapshot()

	; Prisma captures DirectInput key codes and applies them only when Save is pressed.
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "text_chat", "Text Chat", "Open Prisma Text Chat immediately. Holding the key also opens it.", "keymap", _chatbox_focus_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "voice_chat", "Voice Chat", "Hold to talk. Tap to stop current and queued dialogue, or double-tap to make the NPC in your crosshair wait here. With a book open, press to summarize it.", "keymap", _myKey2 as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "halt_ai_actions", "Halt AI Actions", "Immediately stop CHIM actions for the target or nearby NPCs.", "keymap", _halt_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "master_menu", "Master Menu", "Open the CHIM Master Menu.", "keymap", _mastermenu_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "manual_ai_activate", "Manual AI Activate", "Activate or deactivate AI control for the targeted NPC.", "keymap", _myKey7 as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "soulgaze", SoulGazeDisplayName(), "Tap to capture visual context, double-tap an AI NPC for a portrait, or hold for a nearby NPC to describe the scene.", "keymap", _soulgaze_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Primary Hotkeys", "text_chat_deprecated", "Text Chat (Deprecated)", "Tap to type a message in the legacy textbox. Hold to make the NPC in your crosshair wait here. Use Text Chat for Prisma UI.", "keymap", _myKey as String, "0|0|0||0|1")
	PublishPrismaMCMEntry("Hotkeys", "Prisma Hotkeys", "chatbox_view", "Chatbox View", "Toggle the live Prisma Chatbox View.", "keymap", _chatbox_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Prisma Hotkeys", "actions_menu", "Actions Menu", "Open the Prisma AI actions panel.", "keymap", _settingsmenu_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Prisma Hotkeys", "overlay_status_cycle", "Status, Minihud, Terminator Views", "Cycle through the Prisma status views.", "keymap", _overlaystatus_cycle_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Prisma Hotkeys", "history_diaries_cycle", "History/Diaries", "Cycle through Conversation History and Diaries.", "keymap", _historydiaries_cycle_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Prisma Hotkeys", "browser", "Browser Beta", "Toggle the in-game CHIM Browser.", "keymap", _browser_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Prisma Hotkeys", "logs_view", "Logs View (Beta)", "Open the Prisma CHIM Logs View.", "keymap", _debugger_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Hotkeys", "Wheel Menus (Deprecated)", "master_wheel", "Master Wheel", "Deprecated wheel-menu launcher.", "keymap", _masterwheel_key as String, "0|0|0||0|1")
	PublishPrismaMCMEntry("Hotkeys", "Wheel Menus (Deprecated)", "roleplay_wheel", "Roleplay Wheel", "Deprecated roleplay wheel.", "keymap", _myKey4 as String, "0|0|0||0|1")
	PublishPrismaMCMEntry("Hotkeys", "Wheel Menus (Deprecated)", "settings_wheel", "Settings Wheel", "Deprecated settings wheel.", "keymap", _myKey3 as String, "0|0|0||0|1")
	PublishPrismaMCMEntry("Hotkeys", "Wheel Menus (Deprecated)", "mode_wheel", "Mode Wheel", "Deprecated chat-mode wheel.", "keymap", _godmode_key as String, "0|0|0||0|1")
	PublishPrismaMCMEntry("Hotkeys", "Wheel Menus (Deprecated)", "soulgaze_wheel", "Soulgaze Wheel", "Deprecated Soulgaze wheel.", "keymap", _myKey6 as String, "0|0|0||0|1")

	PublishPrismaMCMEntry("Auto Activate", "Auto Activate", "enable_auto_activate", "Enable Auto Activate", "Automatically activate eligible NPCs around the player.", "toggle", PrismaMCMBool(_toggleAddAllNPCState), "0|1|1||0|0")
	PublishPrismaMCMEntry("Auto Activate", "Distances", "max_distance_inside", "Interior Auto Activate Distance", "Auto Activate NPCs within this distance indoors.", "slider", _max_distance_inside as String, "10|5000|1|units|0|0")
	PublishPrismaMCMEntry("Auto Activate", "Distances", "max_distance_outside", "Exterior Auto Activate Distance", "Auto Activate NPCs within this distance outdoors.", "slider", _max_distance_outside as String, "10|5000|1|units|0|0")
	PublishPrismaMCMEntry("Auto Activate", "Hearing", "spatial_hearing_inside", "Interior Spatial Hearing Distance", "Set indoor conversation hearing distance.", "slider", _spatial_hearing_inside as String, "50|5000|1|units|0|0")
	PublishPrismaMCMEntry("Auto Activate", "Hearing", "spatial_hearing_outside", "Exterior Spatial Hearing Distance", "Set outdoor conversation hearing distance.", "slider", _spatial_hearing_outside as String, "50|5000|1|units|0|0")
	PublishPrismaMCMEntry("Auto Activate", "Hearing", "auto_hearing_radius_m", "Auto Hearing Radius", "Direct auto-hearing radius in meters.", "slider", _auto_hearing_radius_m as String, "1|20|1|meters|0|0")
	PublishPrismaMCMEntry("Auto Activate", "Eligibility", "autoadd_hostile", "Add Hostile NPCs", "Allow Auto Activate to include hostile NPCs.", "toggle", PrismaMCMBool(_toggle_autoadd_hostile_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Auto Activate", "Eligibility", "autoadd_creature_npcs", "Add Creature NPCs", "Allow Auto Activate for a set group of creatures such as dragons, giants, Falmer, undead and animal followers. Hostile ones still need Add Hostile NPCs.", "toggle", PrismaMCMBool(_toggle_autoadd_creature_npcs_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Auto Activate", "Eligibility", "autoadd_allraces", "Add All races", "Allow Auto Activate for animals and other normally excluded races.", "toggle", PrismaMCMBool(_toggle_autoadd_allraces_state), "0|1|1||0|0")

	PublishPrismaMCMEntry("Behavior", "Timers", "bored_period", "Bored Event Timer", "Minimum period between potential Bored events.", "slider", _bored_period as String, "15|600|1|seconds|0|0")
	PublishPrismaMCMEntry("Behavior", "Timers", "dynamic_profile_period", "Dynamic Profile Timer", "Period for automatic dynamic profile updates.", "slider", _dynamic_profile_period as String, "5|120|1|minutes|0|0")
	PublishPrismaMCMEntry("Behavior", "General Behavior", "enable_ai_actions", "Enable AI Actions", "Allow AI NPCs to perform actions.", "toggle", PrismaMCMBool(_toggleState2), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "General Behavior", "animations", "Enable Animations", "Allow AI NPCs to perform animations.", "toggle", PrismaMCMBool(_animationstate), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "General Behavior", "player_tts_traditional_dialogue", "Player TTS for Traditional Dialogue", "Play configured Player TTS for traditional dialogue choices.", "toggle", PrismaMCMBool(_playerTtsTraditionalDialogueState), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "General Behavior", "capture_background_chat", "Vanilla Dialogue", "When on, vanilla dialogue-menu conversations and nearby ambient NPC chatter are added to AI context. When off, neither is captured; normal dialogue and subtitles still work.", "toggle", PrismaMCMBool(_captureBackgroundChatState), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "General Behavior", "soulgaze_hd", "Soulgaze HD Mode", "Use DirectX backbuffer capture for Soulgaze.", "toggle", PrismaMCMBool(_toggleState7), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "General Behavior", "timeout", "Connection Timeout", "Timeout for requests to CHIM Server.", "slider", _timeout_int as String, "15|300|1|seconds|0|0")
	PublishPrismaMCMEntry("Behavior", "NPC Behavior", "npc_sandbox_near", "NPCs Sandbox Near Player", "Let NPCs subtly move near the player during conversations.", "toggle", PrismaMCMBool(_toggle_npc_go_near_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "NPC Behavior", "npc_walk_to_target", "NPCs Walk To Target", "Let speaking NPCs walk toward their target.", "toggle", PrismaMCMBool(_toggle_npc_walk_to_target_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "NPC Behavior", "seat_conversation_camera", "Seat Conversation Camera", "Turn the first-person camera toward speaking NPCs while seated.", "toggle", PrismaMCMBool(_toggle_autofocus_on_sit_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "NPC Behavior", "npc_scene_safety", "NPC Scene Safety", "Prevent traditional dialogue-scene NPCs from responding automatically.", "toggle", PrismaMCMBool(_toggle_restrict_onscene_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "Combat Settings", "combat_dialogue", "Allow combat dialogue", "Allow CHIM dialogue while NPCs are in combat.", "toggle", PrismaMCMBool(_toggle_combatdialogue_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "Combat Settings", "cancel_dialogue_on_combat", "Clear dialogue entering combat", "Cancel active AI dialogue when combat starts.", "toggle", PrismaMCMBool(_toggle_cancel_dialogue_on_combat_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "Combat Settings", "combat_barks", "Enable Combat Barks", "Let combatants periodically shout combat barks.", "toggle", PrismaMCMBool(_toggle_combat_barks_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Behavior", "Combat Settings", "combat_barks_period", "Combat Bark Timer", "Seconds between combat barks.", "slider", _combat_barks_period as String, "5|120|5|seconds|0|0")

	PublishPrismaMCMEntry("Sound", "Basic", "sound_volume", "AI Voice Volume", "Set AI NPC speech volume.", "slider", _sound_volume as String, "0|500|2|%|0|0")
	PublishPrismaMCMEntry("Sound", "Basic", "head_voice_volume", "Narrator / Player TTS Volume", "Adjust narrator and player TTS volume relative to AI voices.", "slider", _head_voice_volume as String, "0|200|5|%|0|0")
	PublishPrismaMCMEntry("Sound", "Basic", "audio_mode", "Audio Mode", "3D Advanced: directional with distance fading and muffling. 3D Legacy: directional with legacy distance fading. 2D Flat: directional without distance fading. Mono: non-positional. Mono + Advanced Effects: non-positional with advanced distance fading and muffling.", "enum", _audio_mode as String, "0|4|1||0|0")
	PublishPrismaMCMEntry("Sound", "Basic", "sound_distance_scale", "AI Voice Distance Scale", "Adjust AI NPC playback volume at distance. 3D Advanced and Mono + Advanced Effects only.", "slider", _sound_ds as String, "0.1|20|0.1||" + PrismaMCMReadonly((_audio_mode == 2 || _audio_mode == 4)) + "|0")
	PublishPrismaMCMEntry("Sound", "Basic", "playback_dropoff_inside", "Interior Playback Dropoff", "Indoor playback dropoff aggressiveness. 3D Advanced and Mono + Advanced Effects only.", "slider", _playback_dropoff_inside as String, "25|200|1|%|" + PrismaMCMReadonly((_audio_mode == 2 || _audio_mode == 4)) + "|0")
	PublishPrismaMCMEntry("Sound", "Basic", "playback_dropoff_outside", "Exterior Playback Dropoff", "Outdoor playback dropoff aggressiveness. 3D Advanced and Mono + Advanced Effects only.", "slider", _playback_dropoff_outside as String, "25|200|1|%|" + PrismaMCMReadonly((_audio_mode == 2 || _audio_mode == 4)) + "|0")
	PublishPrismaMCMEntry("Sound", "Basic", "curve_legacy_distance", "3D Legacy Distance Scaler", "How strongly 3D Legacy attenuates voices with distance. Higher values attenuate less. Values below 1 select 2D Flat, retaining direction without distance fading. 3D Legacy only.", "slider", _curve_legacy_distance as String, "0|4|0.1||" + PrismaMCMReadonly(_audio_mode == 1) + "|0")
	PublishPrismaMCMEntry("Sound", "Basic", "camera_based_audio", "Camera Based Audio", "Base 3D voice direction on camera facing. 2D Flat, 3D Legacy and 3D Advanced only.", "toggle", PrismaMCMBool(_camera_based_audio_state), "0|1|1||" + PrismaMCMReadonly(IsAudioMode3D()) + "|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "sound_preclip", "Skip milliseconds at beginning", "Skip silence at the beginning of generated speech.", "slider", _sound_preclip as String, "0|100|10|ms|0|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "sound_postclip", "Skip milliseconds at end", "Skip silence at the end of generated speech.", "slider", _sound_postclip as String, "0|2000|2|ms|0|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "invert_heading", "3D Sound Invert Heading", "Invert 3D audio heading when front and back sound reversed. 2D Flat, 3D Legacy and 3D Advanced only.", "toggle", PrismaMCMBool(_invertheadingstate), "0|1|1||" + PrismaMCMReadonly(IsAudioMode3D()) + "|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "lip_resolution", "Resolution of Lip Animations", "Tune lip animation sampling resolution.", "slider", _lip_res as String, "0|1000|10||0|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "lip_intensity", "Intensity of Lip Animations", "Tune mouth movement intensity.", "slider", _lip_int as String, "0.1|2|0.1||0|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "pause_dialogue", "Pause Dialogue on Game Pause", "Pause CHIM dialogue while game menus pause Skyrim.", "toggle", PrismaMCMBool(_pauseDialogueState), "0|1|1||0|0")
	PublishPrismaMCMEntry("Sound", "Advanced", "use_websocket_stt", "Use WebSocket STT", "Use the optional local WebSocket speech-to-text service.", "toggle", PrismaMCMBool(_toggle_usewebsocketstt_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Sound", "Open Mic Settings", "openmic_enabled", "Enable Open Mic", "Automatically record when voice activity is detected.", "toggle", PrismaMCMBool(_toggle_openmic_state), "0|1|1||0|0")
	PublishPrismaMCMEntry("Sound", "Open Mic Settings", "openmic_sensitivity", "Voice Detection Sensitivity", "Higher values require louder input to start recording.", "slider", _openmic_sensitivity as String, "100|5000|100||0|0")
	PublishPrismaMCMEntry("Sound", "Open Mic Settings", "openmic_enddelay", "End of Sentence Delay", "Wait this long after voice stops before processing speech.", "slider", _openmic_enddelay as String, "0.5|5|0.1|seconds|0|0")
	PublishPrismaMCMEntry("Sound", "Open Mic Settings", "openmic_mute", "Mute Open Mic", "Key used to temporarily mute open microphone capture.", "keymap", _openmic_mute_key as String, "0|0|0||0|0")
	PublishPrismaMCMEntry("Sound", "Recording Device", "current_recording_device", "Current Device", "Windows recording device currently resolved by CHIM.", "text", AIAgentFunctions.getCurrentRecordingDeviceName(), "0|0|0||1|0")

	AIAgentFunctions.commitChimMcmSnapshot(_prismaMcmRevision)
EndFunction

; Prisma label only. The native MCM uses the "$chim_soulgaze_hotkey" translation key so
; SkyUI resolves the casing in Scaleform instead of relying on the Papyrus string table.
; Build the label at runtime so the assembler cannot merge it with the "soulgaze" ID.
; A local variable prevents the optimizer from folding the concatenation into a literal.
String Function SoulGazeDisplayName()
	String prefix = "Soul"
	return prefix + "Gaze"
EndFunction

bool Function IsPrismaMCMKeySetting(String keyName)
	return keyName == "text_chat" || keyName == "voice_chat" || keyName == "halt_ai_actions" || keyName == "master_menu" || keyName == "manual_ai_activate" || keyName == "soulgaze" || keyName == "text_chat_deprecated" || keyName == "chatbox_view" || keyName == "actions_menu" || keyName == "overlay_status_cycle" || keyName == "history_diaries_cycle" || keyName == "browser" || keyName == "logs_view" || keyName == "master_wheel" || keyName == "roleplay_wheel" || keyName == "settings_wheel" || keyName == "mode_wheel" || keyName == "soulgaze_wheel" || keyName == "openmic_mute"
EndFunction

bool Function IsPrismaMCMValueValid(String keyName, float value)
	if IsPrismaMCMKeySetting(keyName)
		int keyCode = value as Int
		return value == (keyCode as Float) && (keyCode == -1 || (keyCode >= 2 && keyCode <= 281))
	elseif keyName == "max_distance_inside" || keyName == "max_distance_outside"
		return value >= 10.0 && value <= 5000.0
	elseif keyName == "spatial_hearing_inside" || keyName == "spatial_hearing_outside"
		return value >= 50.0 && value <= 5000.0
	elseif keyName == "auto_hearing_radius_m"
		return value >= 1.0 && value <= 20.0
	elseif keyName == "bored_period"
		return value >= 15.0 && value <= 600.0
	elseif keyName == "dynamic_profile_period"
		return value >= 5.0 && value <= 120.0
	elseif keyName == "timeout"
		return value >= 15.0 && value <= 300.0
	elseif keyName == "combat_barks_period"
		return value >= 5.0 && value <= 120.0
	elseif keyName == "sound_volume"
		return value >= 0.0 && value <= 500.0
	elseif keyName == "head_voice_volume"
		return value >= 0.0 && value <= 200.0
	elseif keyName == "audio_mode"
		; Only the five exact mode integers are accepted; anything else keeps the current mode.
		int mode = value as Int
		return value == (mode as Float) && mode >= 0 && mode <= 4
	elseif keyName == "sound_distance_scale"
		return value >= 0.1 && value <= 20.0
	elseif keyName == "curve_legacy_distance"
		return value >= 0.0 && value <= 4.0
	elseif keyName == "playback_dropoff_inside" || keyName == "playback_dropoff_outside"
		return value >= 25.0 && value <= 200.0
	elseif keyName == "sound_preclip"
		return value >= 0.0 && value <= 100.0
	elseif keyName == "sound_postclip"
		return value >= 0.0 && value <= 2000.0
	elseif keyName == "lip_resolution"
		return value >= 0.0 && value <= 1000.0
	elseif keyName == "lip_intensity"
		return value >= 0.1 && value <= 2.0
	elseif keyName == "openmic_sensitivity"
		return value >= 100.0 && value <= 5000.0
	elseif keyName == "openmic_enddelay"
		return value >= 0.5 && value <= 5.0
	endif
	return value == 0.0 || value == 1.0
EndFunction

; Apply a Prisma key capture through the same native bindings used by SkyUI MCM.
bool Function ApplyPrismaMCMKeySetting(String keyName, int keyCode)
	if keyName == "text_chat"
		if keyCode != -1 && keyCode == _chatbox_key
			return false
		endif
		controlScript.removeBinding(_chatbox_focus_key)
		_chatbox_focus_key = keyCode
		controlScript.doBinding17(keyCode)
	elseif keyName == "voice_chat"
		controlScript.removeBinding(_myKey2)
		_myKey2 = keyCode
		controlScript.doBinding2(keyCode)
	elseif keyName == "halt_ai_actions"
		controlScript.removeBinding(_halt_key)
		_halt_key = keyCode
		controlScript.doBinding10(keyCode)
	elseif keyName == "master_menu"
		controlScript.removeBinding(_mastermenu_key)
		_mastermenu_key = keyCode
		controlScript.doBinding19(keyCode)
	elseif keyName == "manual_ai_activate"
		controlScript.removeBinding(_myKey7)
		_myKey7 = keyCode
		controlScript.doBinding7(keyCode)
	elseif keyName == "soulgaze"
		controlScript.removeBinding(_soulgaze_key)
		_soulgaze_key = keyCode
		controlScript.doBinding20(keyCode)
	elseif keyName == "text_chat_deprecated"
		controlScript.removeBinding(_myKey)
		_myKey = keyCode
		controlScript.doBinding(keyCode)
	elseif keyName == "chatbox_view"
		if keyCode != -1 && keyCode == _chatbox_focus_key
			return false
		endif
		controlScript.removeBinding(_chatbox_key)
		_chatbox_key = keyCode
		controlScript.doBinding16(keyCode)
	elseif keyName == "actions_menu"
		controlScript.removeBinding(_settingsmenu_key)
		_settingsmenu_key = keyCode
		controlScript.doBinding18(keyCode)
	elseif keyName == "overlay_status_cycle"
		controlScript.removeBinding(_overlaystatus_cycle_key)
		_overlaystatus_cycle_key = keyCode
		controlScript.doBinding12(keyCode)
	elseif keyName == "history_diaries_cycle"
		controlScript.removeBinding(_historydiaries_cycle_key)
		_historydiaries_cycle_key = keyCode
		controlScript.doBinding13(keyCode)
	elseif keyName == "browser"
		controlScript.removeBinding(_browser_key)
		_browser_key = keyCode
		controlScript.doBinding14(keyCode)
	elseif keyName == "logs_view"
		controlScript.removeBinding(_debugger_key)
		_debugger_key = keyCode
		controlScript.doBinding15(keyCode)
	elseif keyName == "master_wheel"
		controlScript.removeBinding(_masterwheel_key)
		_masterwheel_key = keyCode
		controlScript.doBinding11(keyCode)
	elseif keyName == "roleplay_wheel"
		controlScript.removeBinding(_myKey4)
		_myKey4 = keyCode
		controlScript.doBinding4(keyCode)
	elseif keyName == "settings_wheel"
		controlScript.removeBinding(_myKey3)
		_myKey3 = keyCode
		controlScript.doBinding3(keyCode)
	elseif keyName == "mode_wheel"
		controlScript.removeBinding(_godmode_key)
		_godmode_key = keyCode
		controlScript.doBinding8(keyCode)
	elseif keyName == "soulgaze_wheel"
		controlScript.removeBinding(_myKey6)
		_myKey6 = keyCode
		controlScript.doBinding6(keyCode)
	elseif keyName == "openmic_mute"
		controlScript.removeBinding(_openmic_mute_key)
		_openmic_mute_key = keyCode
		controlScript.doBinding9(keyCode)
	else
		return false
	endif
	return true
EndFunction

bool Function ApplyPrismaMCMSetting(String keyName, float value)
	if !IsPrismaMCMValueValid(keyName, value)
		return false
	endif
	if IsPrismaMCMKeySetting(keyName)
		return ApplyPrismaMCMKeySetting(keyName, value as Int)
	endif

	bool enabled = value > 0.5
	if keyName == "enable_auto_activate"
		_toggleAddAllNPCState = enabled
		controlScript.setConf("_toggleAddAllNPC", value)
	elseif keyName == "max_distance_inside"
		_max_distance_inside = value
		controlScript.mdi = value
		controlScript.setConf("_max_distance_inside", value)
	elseif keyName == "max_distance_outside"
		_max_distance_outside = value
		controlScript.mdo = value
		controlScript.setConf("_max_distance_outside", value)
	elseif keyName == "spatial_hearing_inside"
		_spatial_hearing_inside = value
		controlScript.setConf("_spatial_hearing_inside", value)
	elseif keyName == "spatial_hearing_outside"
		_spatial_hearing_outside = value
		controlScript.setConf("_spatial_hearing_outside", value)
	elseif keyName == "auto_hearing_radius_m"
		_auto_hearing_radius_m = value
		controlScript.setConf("_auto_hearing_radius_m", value)
	elseif keyName == "autoadd_hostile"
		_toggle_autoadd_hostile_state = enabled
		controlScript.setConf("_autoadd_hostile", value)
	elseif keyName == "autoadd_allraces"
		_toggle_autoadd_allraces_state = enabled
		controlScript.setConf("_autoadd_allraces", value)
	elseif keyName == "autoadd_creature_npcs"
		_toggle_autoadd_creature_npcs_state = enabled
		controlScript.setConf("_autoadd_creature_npcs", value)
	elseif keyName == "bored_period"
		_bored_period = value
		controlScript.setConf("_bored_period", value)
	elseif keyName == "dynamic_profile_period"
		_dynamic_profile_period = value
		controlScript.setConf("_dynamic_profile_period", value)
	elseif keyName == "enable_ai_actions"
		_toggleState2 = enabled
		controlScript.setNewActionMode(enabled as Int)
	elseif keyName == "animations"
		_animationstate = enabled
		controlScript.setConf("_animations", value)
	elseif keyName == "player_tts_traditional_dialogue"
		_playerTtsTraditionalDialogueState = enabled
		controlScript.setConf("_player_tts_traditional_dialogue", value)
	elseif keyName == "capture_background_chat"
		_captureBackgroundChatState = enabled
		controlScript.setConf("_capture_background_chat", value)
	elseif keyName == "soulgaze_hd"
		_toggleState7 = enabled
		controlScript.setSoulgazeModeNative(enabled as Int)
	elseif keyName == "timeout"
		_timeout_int = value
		controlScript.setConf("_timeout", value)
	elseif keyName == "npc_sandbox_near"
		_toggle_npc_go_near_state = enabled
		StorageUtil.SetIntValue(None, "AIAgentNpcWalkNear", enabled as Int)
	elseif keyName == "npc_walk_to_target"
		_toggle_npc_walk_to_target_state = enabled
		StorageUtil.SetIntValue(None, "AIAgentNpcWalkToTarget", enabled as Int)
	elseif keyName == "seat_conversation_camera"
		_toggle_autofocus_on_sit_state = enabled
		StorageUtil.SetIntValue(None, "AIAgentAutoFocusOnSit", enabled as Int)
	elseif keyName == "npc_scene_safety"
		_toggle_restrict_onscene_state = enabled
		controlScript.setConf("_restrict_onscene", value)
	elseif keyName == "combat_dialogue"
		_toggle_combatdialogue_state = enabled
		controlScript.setConf("_combat_dialogue", value)
	elseif keyName == "cancel_dialogue_on_combat"
		_toggle_cancel_dialogue_on_combat_state = enabled
		controlScript.setConf("_cancel_dialogue_on_combat", value)
	elseif keyName == "combat_barks"
		_toggle_combat_barks_state = enabled
		controlScript.setConf("_combat_barks", value)
	elseif keyName == "combat_barks_period"
		_combat_barks_period = value
		controlScript.setConf("_combat_barks_period", value)
	elseif keyName == "sound_volume"
		_sound_volume = value
		controlScript.setConf("_sound_volume", value)
	elseif keyName == "head_voice_volume"
		_head_voice_volume = value
		controlScript.setConf("_head_voice_volume", value)
	elseif keyName == "sound_distance_scale"
		_sound_ds = value
		controlScript.setConf("_sound_ds", value)
	elseif keyName == "playback_dropoff_inside"
		_playback_dropoff_inside = value
		controlScript.setConf("_playback_dropoff_inside", value)
	elseif keyName == "playback_dropoff_outside"
		_playback_dropoff_outside = value
		controlScript.setConf("_playback_dropoff_outside", value)
	elseif keyName == "audio_mode"
		; Dependent rows are republished right after this returns, so their readonly flags follow
		; the new mode while any other staged edits stay queued in Prisma.
		_audio_mode = value as Int
		ApplyAudioMode()
	elseif keyName == "curve_legacy_distance"
		if ((_curve_legacy_distance as Int) >= 1)
			_legacy_distance_saved = _curve_legacy_distance
		endIf
		_curve_legacy_distance = value
		if (_audio_mode == 1 && (value as Int) < 1)
			_audio_mode = 0
		endIf
		ApplyAudioMode(true)
	elseif keyName == "camera_based_audio"
		_camera_based_audio_state = enabled
		controlScript.setConf("_camera_based_audio", value)
	elseif keyName == "sound_preclip"
		_sound_preclip = value
		controlScript.setConf("_sound_preclip", value)
	elseif keyName == "sound_postclip"
		_sound_postclip = value
		controlScript.setConf("_sound_postclip", value)
	elseif keyName == "invert_heading"
		_invertheadingstate = enabled
		controlScript.setConf("_invertheadingstate", value)
	elseif keyName == "lip_resolution"
		_lip_res = value
		controlScript.setConf("_lip_res", value)
	elseif keyName == "lip_intensity"
		_lip_int = value
		controlScript.setConf("_lip_int", value)
	elseif keyName == "pause_dialogue"
		_pauseDialogueState = enabled
		controlScript.setConf("_pause_dialogue_when_menu_open", value)
	elseif keyName == "use_websocket_stt"
		_toggle_usewebsocketstt_state = enabled
		StorageUtil.SetIntValue(None, "AIAgentWebSockeSTT", enabled as Int)
	elseif keyName == "openmic_enabled"
		_toggle_openmic_state = enabled
		controlScript.setConf("_openmic_enabled", value)
	elseif keyName == "openmic_sensitivity"
		_openmic_sensitivity = value
		controlScript.setConf("_openmic_sensitivity", value)
	elseif keyName == "openmic_enddelay"
		_openmic_enddelay = value
		controlScript.setConf("_openmic_enddelay", value)
	else
		return false
	endif
	return true
EndFunction

Function PublishPrismaMCMAgents()
	AIAgentFunctions.beginChimMcmAgents()
	Actor playerActor = Game.GetPlayer()
	Actor[] activeAgents = AIAgentFunctions.findAllAgents()
	int i = 0
	int published = 0
	while i < activeAgents.Length && published < 120
		if activeAgents[i] && activeAgents[i] != playerActor && activeAgents[i].GetDisplayName() != "The Narrator"
			AIAgentFunctions.publishChimMcmAgent("active", activeAgents[i].GetFormID(), activeAgents[i].GetDisplayName())
			published += 1
		endif
		i += 1
	endwhile

	Actor[] availableAgents = AIAgentFunctions.findAllNearbyNonAgents()
	i = 0
	published = 0
	while i < availableAgents.Length && published < 60
		if availableAgents[i] && availableAgents[i] != playerActor && availableAgents[i].GetDisplayName() != "The Narrator"
			AIAgentFunctions.publishChimMcmAgent("available", availableAgents[i].GetFormID(), availableAgents[i].GetDisplayName())
			published += 1
		endif
		i += 1
	endwhile
	AIAgentFunctions.commitChimMcmAgents()
EndFunction

Event OnPrismaMCMRequest(String eventName, String payload, Float numericValue, Form sender)
	if payload == "snapshot"
		PublishPrismaMCMState()
		return
	elseif payload == "agents_refresh"
		PublishPrismaMCMAgents()
		return
	elseif StringUtil.Find(payload, "set|") == 0
		String keyName = StringUtil.Substring(payload, 4)
		bool applied = ApplyPrismaMCMSetting(keyName, numericValue)
		if applied
			_prismaMcmRevision += 1
			AIAgentFunctions.publishChimMcmCommandResult(payload, true, "Setting applied.")
			PublishPrismaMCMState()
		else
			AIAgentFunctions.publishChimMcmCommandResult(payload, false, "The setting or value is invalid.")
		endif
		return
	elseif payload == "agents_add_all"
		AIAgentFunctions.testAddAllNPCAround()
		AIAgentFunctions.publishChimMcmCommandResult(payload, true, "Nearby AI agents added.")
		PublishPrismaMCMAgents()
		return
	elseif payload == "agents_remove_all"
		AIAgentFunctions.testRemoveAll()
		AIAgentFunctions.publishChimMcmCommandResult(payload, true, "All AI agents removed.")
		PublishPrismaMCMAgents()
		return
	elseif StringUtil.Find(payload, "agent_add|") == 0
		String actorName = StringUtil.Substring(payload, 10)
		Actor[] nearbyActors = AIAgentFunctions.findAllNearbyNonAgents()
		int i = 0
		Actor actorToAdd = None
		while i < nearbyActors.Length && !actorToAdd
			if nearbyActors[i] && nearbyActors[i].GetDisplayName() == actorName
				actorToAdd = nearbyActors[i]
			endif
			i += 1
		endwhile
		if actorToAdd
			AIAgentFunctions.setDrivenByAIA(actorToAdd, true)
			AIAgentFunctions.publishChimMcmCommandResult(payload, true, "AI agent added: " + actorName)
		else
			AIAgentFunctions.publishChimMcmCommandResult(payload, false, "The NPC is no longer nearby.")
		endif
		PublishPrismaMCMAgents()
		return
	elseif StringUtil.Find(payload, "agent_remove|") == 0
		String actorName = StringUtil.Substring(payload, 13)
		AIAgentFunctions.removeAgentByName(actorName)
		AIAgentFunctions.publishChimMcmCommandResult(payload, true, "AI agent removed: " + actorName)
		PublishPrismaMCMAgents()
		return
	elseif payload == "tool|sync_factions_locations"
		AIAgentPapyrusFunctions.RunToolsSendFactionLocationInfo()
		AIAgentFunctions.publishChimMcmCommandResult(payload, true, "Faction and location information sent.")
		return
	elseif payload == "tool|send_voice_samples"
		int voiceResult = AIAgentPapyrusFunctions.RunToolsSendAllVoiceSamples()
		if voiceResult == 0
			AIAgentFunctions.publishChimMcmCommandResult(payload, true, "Voice samples uploaded.")
		else
			AIAgentFunctions.publishChimMcmCommandResult(payload, false, "Voice sample upload failed.")
		endif
		return
	endif
	AIAgentFunctions.publishChimMcmCommandResult(payload, false, "Unknown CHIM MCM command.")
EndEvent

event OnPageReset(string a_page)

	SetCursorFillMode(LEFT_TO_Right)
	
	
	if (a_page=="Hotkeys" || a_page=="Main" || a_page=="")
		AddHeaderOption("Primary Hotkeys")
		AddEmptyOption()
		_keymap_chatbox_focus = AddKeyMapOption("Text Chat", _chatbox_focus_key)
		_keymapOID_K2 = AddKeyMapOption("Voice Chat", _myKey2)
		_keymap_halt = AddKeyMapOption("Halt AI Actions", _halt_key)
		_keymap_mastermenu = AddKeyMapOption("Master Menu", _mastermenu_key)
		_keymapOID_K7 = AddKeyMapOption("Manual AI Activate", _myKey7)
		_keymap_soulgaze = AddKeyMapOption("$chim_soulgaze_hotkey", _soulgaze_key)
		_keymapOID_K = AddKeyMapOption("Text Chat (Deprecated)", _myKey)

		AddEmptyOption()
		AddHeaderOption("Prisma Hotkeys")
		AddEmptyOption()
		_keymap_chatbox = AddKeyMapOption("Chatbox View", _chatbox_key)
		_keymap_settingsmenu = AddKeyMapOption("Actions Menu", _settingsmenu_key)
		_keymap_overlaystatus_cycle = AddKeyMapOption("Status, Minihud, Terminator Views", _overlaystatus_cycle_key)
		_keymap_historydiaries_cycle = AddKeyMapOption("History/Diaries", _historydiaries_cycle_key)
		_keymap_browser = AddKeyMapOption("Browser Beta", _browser_key)
		_keymap_debugger = AddKeyMapOption("Logs View (Beta)", _debugger_key)

		;AddEmptyOption()
		AddHeaderOption("Wheel Menus (Deprecated)")
		AddEmptyOption()
		_keymap_masterwheel = AddKeyMapOption("Master Wheel", _masterwheel_key)
		_keymapOID_K4 = AddKeyMapOption("Roleplay Wheel", _myKey4)
		_keymapOID_K3 = AddKeyMapOption("Settings Wheel", _myKey3)
		_keymap_godmode = AddKeyMapOption("Mode Wheel", _godmode_key)
		_keymapOID_K6 = AddKeyMapOption("Soulgaze Wheel", _myKey6)
	endif
	

	if (a_page=="Auto Activate")
		_toggleAddAllNPC		= AddToggleOption("Enable Auto Activate", _toggleAddAllNPCState)
		AddEmptyOption()
		
		_slider_max_distance_inside	= AddSliderOption("Interior Auto Activate Distance",_max_distance_inside,"{0}" )
		_slider_max_distance_outside	= AddSliderOption("Exterior Auto Activate Distance",_max_distance_outside,"{0}" )
		_slider_spatial_hearing_inside	= AddSliderOption("Interior Spatial Hearing Distance",_spatial_hearing_inside,"{0}" )
		_slider_spatial_hearing_outside	= AddSliderOption("Exterior Spatial Hearing Distance",_spatial_hearing_outside,"{0}" )
		_slider_auto_hearing_radius_m	= AddSliderOption("Auto Hearing Radius",_auto_hearing_radius_m,"{0}" )
		
		AddEmptyOption()
		
		_toggle_autoadd_hostile	= AddToggleOption("Add Hostile NPCs", _toggle_autoadd_hostile_state)
		AddEmptyOption() 
		_toggle_autoadd_creature_npcs	= AddToggleOption("Add Creature NPCs", _toggle_autoadd_creature_npcs_state)
		AddEmptyOption()
		_toggle_autoadd_allraces	= AddToggleOption("Add All races", _toggle_autoadd_allraces_state)
		
	endif

	if (a_page=="Behavior")
		_slider_bored_period	= AddSliderOption("Bored Event Timer (seconds)",_bored_period,"{0}" )
		_slider_dynamic_profile_period	= AddSliderOption("Dynamic Profile Timer (minutes)",_dynamic_profile_period,"{0}" )
		
		;AddEmptyOption()
		AddHeaderOption("General Behavior")
		AddEmptyOption()

		_toggle1OID_C = AddToggleOption("Enable AI Actions", _toggleState2)
		_toggleAnimation = AddToggleOption("Enable Animations", _animationstate)
		_togglePlayerTtsTraditionalDialogue = AddToggleOption("Player TTS for Traditional Dialogue", _playerTtsTraditionalDialogueState)
		_toggleCaptureBackgroundChat = AddToggleOption("Vanilla Dialogue", _captureBackgroundChatState)
		_toggle1OID_E = AddToggleOption("Soulgaze HD Mode", _toggleState7)
		_slider_timeout = AddSliderOption("Connection Timeout (seconds)", _timeout_int, "{1}")
		_slider_maintenance_period = AddSliderOption("Maintenance period", _maintenance_period, "{0}")

		;AddEmptyOption()
		AddHeaderOption("NPC Behavior")
		AddEmptyOption()
		
		_toggle_npc_go_near	= AddToggleOption("NPCs Sandbox Near Player", _toggle_npc_go_near_state)
		_toggle_npc_walk_to_target	= AddToggleOption("NPCs Walk To Target", _toggle_npc_walk_to_target_state)
		_toggle_autofocus_on_sit	= AddToggleOption("Seat Conversation Camera", _toggle_autofocus_on_sit_state)
		
		_toggle_restrict_onscene	= AddToggleOption("NPC Scene Safety", _toggle_restrict_onscene_state)
		
		;AddEmptyOption()
		AddHeaderOption("Combat Settings")
		AddEmptyOption()
		
		_toggle_combatdialogue	= AddToggleOption("Allow combat dialogue", _toggle_combatdialogue_state)
		_toggle_cancel_dialogue_on_combat = AddToggleOption("Clear dialogue entering combat", _toggle_cancel_dialogue_on_combat_state)
		;AddEmptyOption()
		_toggle_combat_barks = AddToggleOption("Enable Combat Barks", _toggle_combat_barks_state)
		_slider_combat_barks_period = AddSliderOption("Combat Bark Timer (seconds)", _combat_barks_period, "{0}")
		
	endif
	

	if (a_page=="Sound")
		MigrateAudioMode()
		; Volume is always available. Everything else is greyed out unless the mode uses it; the
		; stored values are untouched so switching modes back restores them.
		bool audioIs3D = IsAudioMode3D()
		bool audioIsLegacy = _audio_mode == 1
		bool audioIsAdvanced = _audio_mode == 2 || _audio_mode == 4

		AddHeaderOption("Basic")
		AddEmptyOption()
		_slider_volume		= AddSliderOption("AI Voice Volume", _sound_volume,"{0}")
		_slider_head_voice_volume = AddSliderOption("Narrator / Player TTS Volume (%)", _head_voice_volume,"{0}")
		_menu_audio_mode	= AddMenuOption("Audio Mode", AudioModeLabel(_audio_mode))
		_slider_ds			= AddSliderOption("AI Voice Distance Scale",_sound_ds,"{1}", AudioModeOptionFlags(audioIsAdvanced))
		_slider_playback_dropoff_inside = AddSliderOption("Interior Playback Dropoff (%)", _playback_dropoff_inside, "{0}", AudioModeOptionFlags(audioIsAdvanced))
		_slider_playback_dropoff_outside = AddSliderOption("Exterior Playback Dropoff (%)", _playback_dropoff_outside, "{0}", AudioModeOptionFlags(audioIsAdvanced))
		_slider_curve_legacy_distance = AddSliderOption("3D Legacy Distance Scaler", _curve_legacy_distance, "{1}", AudioModeOptionFlags(audioIsLegacy))
		_toggleCameraBasedAudio = AddToggleOption("Camera Based Audio", _camera_based_audio_state, AudioModeOptionFlags(audioIs3D))
		; Two fillers keep the option count even so the Advanced header stays in the left column.
		AddEmptyOption()
		AddEmptyOption()

		AddHeaderOption("Advanced")
		AddEmptyOption()
		_slider_preclip		= AddSliderOption("Skip milliseconds at begining",_sound_preclip,"{0}" )
		_slider_postclip	= AddSliderOption("Skip milliseconds at end",_sound_postclip,"{0}" )
		
		_toggleInvertHeading	= AddToggleOption("3D Sound Invert Heading",_invertheadingstate, AudioModeOptionFlags(audioIs3D))
		
		_slider_lip_res	= AddSliderOption("Resolution of Lip Animations",_lip_res,"{0}" )
		_slider_lip_int			= AddSliderOption("Intensity of Lip Animations ",_lip_int,"{1}" )
		
		_togglePauseDialogue	= AddToggleOption("Pause Dialogue on Game Pause",_pauseDialogueState)
		_toggle_usewebsocketstt			= AddToggleOption("Use WebSocket STT",_toggle_usewebsocketstt_state)
		
		AddEmptyOption()
		AddHeaderOption("Open Mic Settings")
		_toggle_openmic		= AddToggleOption("Enable Open Mic", _toggle_openmic_state)
		_slider_openmic_sensitivity = AddSliderOption("Voice Detection Sensitivity", _openmic_sensitivity, "{0}")
		_slider_openmic_enddelay = AddSliderOption("End of Sentence Delay (seconds)", _openmic_enddelay, "{1}")
		_keymap_openmic_mute = AddKeyMapOption("Mute Open Mic", _openmic_mute_key)

		AddEmptyOption()
		AddHeaderOption("Recording Device")
		_text_current_recording_device = AddTextOption("Current Device", AIAgentFunctions.getCurrentRecordingDeviceName())

	
	endif
	
	if (a_page=="AI Agents")
		AddHeaderOption("Agent Management")
		AddEmptyOption()
		
		_toggleAddAllNowNPC	= AddToggleOption("Add all current AI Agents", false)
		_removeAllAgentsOID = AddToggleOption("Remove All AI Agents", false)
		AddEmptyOption()
		
		; Get current AI agents (all agents, not just nearby)
		Actor[] allAgents = AIAgentFunctions.findAllAgents()
		_currentAgentNames = new string[128]  ; Maximum agents we can display
		_agentToggleOIDs = new int[128]
		
		int i = 0
		int displayedAgents = 0
		while i < allAgents.Length && displayedAgents < 120  ; Leave some room for other options
			if (allAgents[i] && allAgents[i].GetDisplayName() != "The Narrator" && allAgents[i] != Game.GetPlayer())
				_currentAgentNames[displayedAgents] = allAgents[i].GetDisplayName()
				_agentToggleOIDs[displayedAgents] = AddToggleOption("Remove: " + _currentAgentNames[displayedAgents], false)
				displayedAgents += 1
			endif
			i += 1
		endwhile
		
		AddHeaderOption("Active AI Agents")
		AddEmptyOption()
		
		if (displayedAgents == 0)
			AddTextOption("No AI agents active", "")
		else
			AddTextOption("Total Active Agents: " + displayedAgents, "")
		endif
		
		AddEmptyOption()
		AddHeaderOption("Nearby Available NPCs")
		AddEmptyOption()
		
		_refreshNearbyNPCsOID = AddToggleOption("Refresh Nearby NPCs List", false)
		AddEmptyOption()
		
		; Get nearby NPCs that are NOT currently AI agents
		Actor[] nearbyNonAgents = AIAgentFunctions.findAllNearbyNonAgents()
		_nearbyNpcNames = new string[128]
		_nearbyNpcToggleOIDs = new int[128]
		
		int j = 0
		int displayedNearbyNPCs = 0
		while j < nearbyNonAgents.Length && displayedNearbyNPCs < 60  ; Limit to reasonable number
			if (nearbyNonAgents[j] && nearbyNonAgents[j].GetDisplayName() != "The Narrator" && nearbyNonAgents[j] != Game.GetPlayer())
				_nearbyNpcNames[displayedNearbyNPCs] = nearbyNonAgents[j].GetDisplayName()
				_nearbyNpcToggleOIDs[displayedNearbyNPCs] = AddToggleOption("Add: " + _nearbyNpcNames[displayedNearbyNPCs], false)
				displayedNearbyNPCs += 1
			endif
			j += 1
		endwhile
		
		if (displayedNearbyNPCs == 0)
			AddTextOption("No nearby available NPCs", "")
		else
			AddTextOption("Available NPCs: " + displayedNearbyNPCs, "")
		endif
		
	endif
	
	if (a_page=="Tools")
		
		_actionSendLocations = AddToggleOption("Send faction and location info", false)
		_actionSendVoices = AddToggleOption("Send all voice samples to server", false)
	
	endif

endEvent

event OnOptionMenuOpen(int a_option)
	{Called when the user selects a menu option}

	if (a_option == _menu_audio_mode)
		; Old saves reach this with no option strings and, on the very first open, no resolved
		; mode either, so both are rebuilt before the dialog reads them.
		MigrateAudioMode()
		SetMenuDialogOptions(AudioModeOptions())
		SetMenuDialogStartIndex(AudioModeMenuIndex(_audio_mode))
		SetMenuDialogDefaultIndex(AudioModeMenuIndex(_audio_modeDefault))
	endIf

endEvent

event OnOptionMenuAccept(int a_option, int a_index)
	if (a_option == _menu_audio_mode)
		if (a_index < 0 || a_index > 4)
			return
		endIf
		_audio_mode = AudioModeMenuIndex(a_index)
		ApplyAudioMode()
		SetMenuOptionValue(a_option, AudioModeLabel(_audio_mode))
		_prismaMcmRevision += 1
		PublishPrismaMCMState()
		; Redraw so the mode-dependent rows pick up their new enabled state.
		ForcePageReset()
	endIf
endEvent

event OnOptionSliderOpen(int a_option)
	{Called when the user selects a slider option}

	if (a_option == _slider_volume)
		SetSliderDialogStartValue(_sound_volume)
		SetSliderDialogDefaultValue(50)
		SetSliderDialogRange(0, 500)
		SetSliderDialogInterval(2)
	endIf

	if (a_option == _slider_head_voice_volume)
		SetSliderDialogStartValue(_head_voice_volume)
		SetSliderDialogDefaultValue(100)
		SetSliderDialogRange(0, 200)
		SetSliderDialogInterval(5)
	endIf
	
	if (a_option == _slider_preclip)
		SetSliderDialogStartValue(_sound_preclip)
		SetSliderDialogDefaultValue(100)
		SetSliderDialogRange(0, 100)
		SetSliderDialogInterval(10)
	endIf
	
	if (a_option == _slider_postclip)
		SetSliderDialogStartValue(_sound_postclip)
		SetSliderDialogDefaultValue(1000)
		SetSliderDialogRange(0, 2000)
		SetSliderDialogInterval(2)
	endIf
	if (a_option == _slider_ds)
		if (_sound_ds < 0.1)
			_sound_ds = 0.1
		elseif (_sound_ds > 20.0)
			_sound_ds = 20.0
		endIf
		SetSliderDialogStartValue(_sound_ds)
		SetSliderDialogDefaultValue(2)
		SetSliderDialogRange(0.1, 20.0)
		SetSliderDialogInterval(0.1)
	endIf
	if (a_option == _slider_playback_dropoff_inside)
		SetSliderDialogStartValue(_playback_dropoff_inside)
		SetSliderDialogDefaultValue(70)
		SetSliderDialogRange(25, 200)
		SetSliderDialogInterval(1)
	endIf
	if (a_option == _slider_playback_dropoff_outside)
		SetSliderDialogStartValue(_playback_dropoff_outside)
		SetSliderDialogDefaultValue(70)
		SetSliderDialogRange(25, 200)
		SetSliderDialogInterval(1)
	endIf
	if (a_option == _slider_lip_int)
		SetSliderDialogStartValue(_lip_int)
		SetSliderDialogDefaultValue(1.0)
		SetSliderDialogRange(0.1, 2)
		SetSliderDialogInterval(0.1)
	endIf
	if (a_option == _slider_lip_res)
		SetSliderDialogStartValue(_lip_res)
		SetSliderDialogDefaultValue(500)
		SetSliderDialogRange(0, 1000)
		SetSliderDialogInterval(10)
	endIf
	
	if (a_option == _slider_timeout)
		SetSliderDialogStartValue(_timeout_int)
		SetSliderDialogDefaultValue(30)
		SetSliderDialogRange(15, 300)
		SetSliderDialogInterval(1)
	endIf
	
	if (a_option == _slider_max_distance_inside)
		SetSliderDialogStartValue(_max_distance_inside)
		SetSliderDialogDefaultValue(1200)
		SetSliderDialogRange(10, 5000)
		SetSliderDialogInterval(1)
	endIf
	
	if (a_option == _slider_max_distance_outside)
		SetSliderDialogStartValue(_max_distance_outside)
		SetSliderDialogDefaultValue(2400)
		SetSliderDialogRange(10, 5000)
		SetSliderDialogInterval(1)
	endIf

	if (a_option == _slider_spatial_hearing_inside)
		SetSliderDialogStartValue(_spatial_hearing_inside)
		SetSliderDialogDefaultValue(471)
		SetSliderDialogRange(50, 5000)
		SetSliderDialogInterval(1)
	endIf

	if (a_option == _slider_spatial_hearing_outside)
		SetSliderDialogStartValue(_spatial_hearing_outside)
		SetSliderDialogDefaultValue(1018)
		SetSliderDialogRange(50, 5000)
		SetSliderDialogInterval(1)
	endIf

	if (a_option == _slider_auto_hearing_radius_m)
		SetSliderDialogStartValue(_auto_hearing_radius_m)
		SetSliderDialogDefaultValue(8)
		SetSliderDialogRange(1, 20)
		SetSliderDialogInterval(1)
	endIf
	
	if (a_option == _slider_bored_period)
		SetSliderDialogStartValue(_bored_period)
		SetSliderDialogDefaultValue(60)
		SetSliderDialogRange(15, 600)
		SetSliderDialogInterval(1)
	endIf
	
	if (a_option == _slider_dynamic_profile_period)
		SetSliderDialogStartValue(_dynamic_profile_period)
		SetSliderDialogDefaultValue(20)
		SetSliderDialogRange(5, 120)
		SetSliderDialogInterval(1)
	endIf
	
	if (a_option == _slider_openmic_sensitivity)
		SetSliderDialogStartValue(_openmic_sensitivity)
		SetSliderDialogDefaultValue(1000)
		SetSliderDialogRange(100, 5000)
		SetSliderDialogInterval(100)
	endIf
	
	if (a_option == _slider_openmic_enddelay)
		SetSliderDialogStartValue(_openmic_enddelay)
		SetSliderDialogDefaultValue(1.0)
		SetSliderDialogRange(0.5, 5.0)
		SetSliderDialogInterval(0.1)
	endIf
	
	if (a_option == _slider_combat_barks_period)
		SetSliderDialogStartValue(_combat_barks_period)
		SetSliderDialogDefaultValue(30.0)
		SetSliderDialogRange(5.0, 120.0)
		SetSliderDialogInterval(5.0)
	endIf
	
	if (a_option == _slider_curve_legacy_distance)
		SetSliderDialogStartValue(_curve_legacy_distance)
		SetSliderDialogDefaultValue(1.0)
		SetSliderDialogRange(0.0, 4.0)
		SetSliderDialogInterval(0.1)
	endIf
	
	if (a_option == _slider_maintenance_period)
		SetSliderDialogStartValue(_maintenance_period)
		SetSliderDialogDefaultValue(4)
		SetSliderDialogRange(4, 60)
		SetSliderDialogInterval(1)
	endIf
	

endEvent

event OnOptionSliderAccept(int a_option, float a_value)
	if (a_option == _slider_volume)
		_sound_volume = a_value
		controlScript.setConf("_sound_volume",a_value)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	if (a_option == _slider_head_voice_volume)
		_head_voice_volume = a_value
		controlScript.setConf("_head_voice_volume", a_value)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
		if (a_option == _slider_preclip)
		_sound_preclip = a_value
		controlScript.setConf("_sound_preclip",a_value)
		SetSliderOptionValue(a_option, a_value, "{0}")

	endIf
	if (a_option == _slider_postclip)
		_sound_postclip = a_value
		controlScript.setConf("_sound_postclip",a_value)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	if (a_option == _slider_ds)
		_sound_ds = a_value
		controlScript.setConf("_sound_ds",a_value)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	if (a_option == _slider_playback_dropoff_inside)
		_playback_dropoff_inside = a_value
		controlScript.setConf("_playback_dropoff_inside", _playback_dropoff_inside)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	if (a_option == _slider_playback_dropoff_outside)
		_playback_dropoff_outside = a_value
		controlScript.setConf("_playback_dropoff_outside", _playback_dropoff_outside)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	if (a_option == _slider_lip_int)
		_lip_int = a_value
		controlScript.setConf("_lip_int",_lip_int)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	if (a_option == _slider_lip_res)
		_lip_res = a_value
		controlScript.setConf("_lip_res",_lip_res)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	
	if (a_option == _slider_timeout)
		_timeout_int = a_value
		controlScript.setConf("_timeout",_timeout_int)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	
	if (a_option == _slider_max_distance_inside)
		_max_distance_inside = a_value
		controlScript.setConf("_max_distance_inside",_max_distance_inside)
		controlScript.mdi=_max_distance_inside;
		SetSliderOptionValue(a_option, a_value, "{1}")
		
	endIf
	
	if (a_option == _slider_max_distance_outside)
		_max_distance_outside = a_value
		controlScript.setConf("_max_distance_outside",_max_distance_outside)
		controlScript.mdo=_max_distance_outside;
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf

	if (a_option == _slider_spatial_hearing_inside)
		_spatial_hearing_inside = a_value
		controlScript.setConf("_spatial_hearing_inside",_spatial_hearing_inside)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf

	if (a_option == _slider_spatial_hearing_outside)
		_spatial_hearing_outside = a_value
		controlScript.setConf("_spatial_hearing_outside",_spatial_hearing_outside)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf

	if (a_option == _slider_auto_hearing_radius_m)
		_auto_hearing_radius_m = a_value
		controlScript.setConf("_auto_hearing_radius_m",_auto_hearing_radius_m)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	
	if (a_option == _slider_bored_period)
		_bored_period = a_value
		controlScript.setConf("_bored_period",_bored_period)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	
	if (a_option == _slider_dynamic_profile_period)
		_dynamic_profile_period = a_value
		controlScript.setConf("_dynamic_profile_period",_dynamic_profile_period)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	
	if (a_option == _slider_openmic_sensitivity)
		_openmic_sensitivity = a_value
		controlScript.setConf("_openmic_sensitivity",_openmic_sensitivity)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	
	if (a_option == _slider_openmic_enddelay)
		_openmic_enddelay = a_value
		controlScript.setConf("_openmic_enddelay",_openmic_enddelay)
		SetSliderOptionValue(a_option, a_value, "{1}")
	endIf
	
	if (a_option == _slider_combat_barks_period)
		_combat_barks_period = a_value
		controlScript.setConf("_combat_barks_period",_combat_barks_period)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	
	if (a_option == _slider_curve_legacy_distance)
		if ((_curve_legacy_distance as Int) >= 1)
			_legacy_distance_saved = _curve_legacy_distance
		endIf
		_curve_legacy_distance = a_value
		if (_audio_mode == 1 && (a_value as Int) < 1)
			_audio_mode = 0
		endIf
		ApplyAudioMode(true)
		_prismaMcmRevision += 1
		PublishPrismaMCMState()
		ForcePageReset()
	endIf
	
	if (a_option == _slider_maintenance_period)
		_maintenance_period = a_value
		controlScript.setConf("_maintenance_period",_maintenance_period)
		SetSliderOptionValue(a_option, a_value, "{0}")
	endIf
	
	
	_prismaMcmRevision += 1
endEvent
	
	
event OnGameReload()
	Debug.Trace("[CHIM] OnGameReload")
	parent.OnGameReload()
	RegisterPrismaMCMEvent()
	bool a; to avoid warnings on runtime
	if (_toggleState1)
		;controlScript.setTTSOn();
		getActionMode();
	endIf
	if (_toggleState3)
		a=controlScript.setVoiceType(1);	
	else
		a=controlScript.setVoiceType(0);
	endif

	a=controlScript.setConf("_sound_postclip",_sound_postclip)
	a=controlScript.setConf("_sound_preclip",_sound_preclip)
	a=controlScript.setConf("_sound_volume",_sound_volume)
	a=controlScript.setConf("_head_voice_volume",_head_voice_volume)
	if (_sound_ds < 0.1)
		_sound_ds = 0.1
	elseif (_sound_ds > 20.0)
		_sound_ds = 20.0
	endIf
	a=controlScript.setConf("_sound_ds",_sound_ds)
	a=controlScript.setConf("_playback_dropoff_inside",_playback_dropoff_inside)
	a=controlScript.setConf("_playback_dropoff_outside",_playback_dropoff_outside)
	ApplyAudioMode(true)

	if (_camera_based_audio_state)
		a=controlScript.setConf("_camera_based_audio",1)
	else
		a=controlScript.setConf("_camera_based_audio",0)
	endif
	if (_invertheadingstate)
		a=controlScript.setConf("_invertheadingstate",1)
	else
		a=controlScript.setConf("_invertheadingstate",0)
	endif
	a=controlScript.setConf("_lip_int",_lip_int)
	a=controlScript.setConf("_lip_res",_lip_res)
	a=controlScript.setConf("_timeout",_timeout_int)


	a=controlScript.setConf("_max_distance_inside",_max_distance_inside)
	a=controlScript.setConf("_max_distance_outside",_max_distance_outside)
	a=controlScript.setConf("_spatial_hearing_inside",_spatial_hearing_inside)
	a=controlScript.setConf("_spatial_hearing_outside",_spatial_hearing_outside)
	a=controlScript.setConf("_auto_hearing_radius_m",_auto_hearing_radius_m)
	
	a=controlScript.setConf("_curve_legacy_distance",_curve_legacy_distance)
	a=controlScript.setConf("_maintenance_period",_maintenance_period)
	
	controlScript.mdi=_max_distance_inside;
	controlScript.mdo=_max_distance_outside;

	
	a=controlScript.setConf("_bored_period",_bored_period)
	a=controlScript.setConf("_dynamic_profile_period",_dynamic_profile_period)
	
	if (_toggleAddAllNPCState)
		a=controlScript.setConf("_toggleAddAllNPC",1)
	else
		a=controlScript.setConf("_toggleAddAllNPC",0)
	endif

	_rechat_policy_asap = true
	a=controlScript.setConf("_rechat_policy_asap",0)

	
	if (_animationstate)
		a=controlScript.setConf("_animations",1)
	else
		a=controlScript.setConf("_animations",0)
	endif
	
	if (_pauseDialogueState)
		a=controlScript.setConf("_pause_dialogue_when_menu_open",1)
	else
		a=controlScript.setConf("_pause_dialogue_when_menu_open",0)
	endif

	if (_playerTtsTraditionalDialogueState)
		a=controlScript.setConf("_player_tts_traditional_dialogue",1)
	else
		a=controlScript.setConf("_player_tts_traditional_dialogue",0)
	endif

	if (_captureBackgroundChatState)
		a=controlScript.setConf("_capture_background_chat",1)
	else
		a=controlScript.setConf("_capture_background_chat",0)
	endif
	
	if (_toggle_autoadd_hostile_state)
		a=controlScript.setConf("_autoadd_hostile",1)
	else
		a=controlScript.setConf("_autoadd_hostile",0)
	endif
	
	if (_toggle_autoadd_allraces_state)
		a=controlScript.setConf("_autoadd_allraces",1)
	else
		a=controlScript.setConf("_autoadd_allraces",0)
	endif
	
	if (_toggle_autoadd_creature_npcs_state)
		a=controlScript.setConf("_autoadd_creature_npcs",1)
	else
		a=controlScript.setConf("_autoadd_creature_npcs",0)
	endif

	a=controlScript.setSoulgazeModeNative(_toggleState7 as Int)
	
	a=controlScript.setConf("_godmode",0)
	controlScript._currentGodmodeStatus=false
	
	; Open mic settings
	if (_toggle_openmic_state)
		a=controlScript.setConf("_openmic_enabled",1)
	else
		a=controlScript.setConf("_openmic_enabled",0)
	endif
	
	a=controlScript.setConf("_openmic_sensitivity",_openmic_sensitivity)
	a=controlScript.setConf("_openmic_enddelay",_openmic_enddelay)
	
	; Combat settings
	if (_toggle_combatdialogue_state)
		a=controlScript.setConf("_combat_dialogue",1)
	else
		a=controlScript.setConf("_combat_dialogue",0)
	endif
	
	if (_toggle_cancel_dialogue_on_combat_state)
		a=controlScript.setConf("_cancel_dialogue_on_combat",1)
	else
		a=controlScript.setConf("_cancel_dialogue_on_combat",0)
	endif
	
	if (_toggle_combat_barks_state)
		a=controlScript.setConf("_combat_barks",1)
	else
		a=controlScript.setConf("_combat_barks",0)
	endif
	
	a=controlScript.setConf("_combat_barks_period",_combat_barks_period)
	
	if (_toggle_restrict_onscene_state)
		a=controlScript.setConf("_restrict_onscene",1)
	else
		a=controlScript.setConf("_restrict_onscene",0)
	endif
	_prismaMcmRevision += 1
endEvent

event OnOptionDefault(int a_option)
	if (a_option == _keymapOID_K)
		controlScript.removeBinding(_myKey)
		_myKey = _myKeyDefault
		SetKeymapOptionValue(a_option, _myKey)
		controlScript.doBinding(_myKey)

	elseif (a_option == _keymapOID_K2)
		controlScript.removeBinding(_myKey2)
		_myKey2 = _myKey2Default
		SetKeymapOptionValue(a_option, _myKey2)
		controlScript.doBinding2(_myKey2)

	elseif (a_option == _keymapOID_K3)
		controlScript.removeBinding(_myKey3)
		_myKey3 = _myKey3Default
		SetKeymapOptionValue(a_option, _myKey3)
		controlScript.doBinding3(_myKey3)

	elseif (a_option == _keymapOID_K4)
		controlScript.removeBinding(_myKey4)
		_myKey4 = _myKey4Default
		SetKeymapOptionValue(a_option, _myKey4)
		controlScript.doBinding4(_myKey4)

	elseif (a_option == _keymapOID_K5)
		controlScript.removeBinding(_myKey5)
		_myKey5 = _myKey5Default
		SetKeymapOptionValue(a_option, _myKey5)
		controlScript.doBinding5(_myKey5)

	elseif (a_option == _keymapOID_K6)
		controlScript.removeBinding(_myKey6)
		_myKey6 = _myKey6Default
		SetKeymapOptionValue(a_option, _myKey6)
		controlScript.doBinding6(_myKey6)

	elseif (a_option == _keymapOID_K7)
		controlScript.removeBinding(_myKey7)
		_myKey7 = _myKey7Default
		SetKeymapOptionValue(a_option, _myKey7)
		controlScript.doBinding7(_myKey7)

	elseif (a_option == _keymap_soulgaze)
		controlScript.removeBinding(_soulgaze_key)
		_soulgaze_key = _soulgaze_keyDefault
		SetKeymapOptionValue(a_option, _soulgaze_key)
		controlScript.doBinding20(_soulgaze_key)

	elseif (a_option == _toggle1OID_C)
		_toggleState2 = _toggleState2Default
		SetToggleOptionValue(a_option, _toggleState2)

	elseif (a_option == _slider_timeout)
		_timeout_int = _timeout_intDefault
		SetSliderOptionValue(a_option, _timeout_int, "{1}")

	elseif (a_option == _toggleAnimation)
		_animationstate = _animationstateDefault
		SetToggleOptionValue(a_option, _animationstate)

	elseif (a_option == _toggle1OID_E)
		_toggleState7 = _toggleState7Default
		SetToggleOptionValue(a_option, _toggleState7)

	elseif (a_option == _slider_volume)
		_sound_volume = _sound_volumeDefault
		SetSliderOptionValue(a_option, _sound_volume, "{1}")

	elseif (a_option == _slider_head_voice_volume)
		_head_voice_volume = _head_voice_volumeDefault
		controlScript.setConf("_head_voice_volume", _head_voice_volume)
		SetSliderOptionValue(a_option, _head_voice_volume, "{0}")

	elseif (a_option == _slider_ds)
		_sound_ds = _sound_dsDefault
		controlScript.setConf("_sound_ds", _sound_ds)
		SetSliderOptionValue(a_option, _sound_ds, "{1}")

	elseif (a_option == _toggle_combatdialogue)
		_toggle_combatdialogue_state = true
		controlScript.setConf("_combat_dialogue", 1)
		SetToggleOptionValue(a_option, _toggle_combatdialogue_state)

	elseif (a_option == _toggle_cancel_dialogue_on_combat)
		_toggle_cancel_dialogue_on_combat_state = _toggle_cancel_dialogue_on_combat_stateDefault
		if (_toggle_cancel_dialogue_on_combat_state)
			controlScript.setConf("_cancel_dialogue_on_combat", 1)
		else
			controlScript.setConf("_cancel_dialogue_on_combat", 0)
		endif
		SetToggleOptionValue(a_option, _toggle_cancel_dialogue_on_combat_state)

	elseif (a_option == _toggle_combat_barks)
		_toggle_combat_barks_state = _toggle_combat_barks_stateDefault
		if (_toggle_combat_barks_state)
			controlScript.setConf("_combat_barks", 1)
		else
			controlScript.setConf("_combat_barks", 0)
		endif
		SetToggleOptionValue(a_option, _toggle_combat_barks_state)

	elseif (a_option == _slider_playback_dropoff_inside)
		_playback_dropoff_inside = _playback_dropoff_insideDefault
		SetSliderOptionValue(a_option, _playback_dropoff_inside, "{0}")

	elseif (a_option == _slider_playback_dropoff_outside)
		_playback_dropoff_outside = _playback_dropoff_outsideDefault
		SetSliderOptionValue(a_option, _playback_dropoff_outside, "{0}")

	elseif (a_option == _slider_preclip)
		_sound_preclip = _sound_preclipDefault
		SetSliderOptionValue(a_option, _sound_preclip, "{1}")

	elseif (a_option == _slider_postclip)
		_sound_postclip = _sound_postclipDefault
		SetSliderOptionValue(a_option, _sound_postclip, "{1}")

	elseif (a_option == _slider_lip_res)
		_lip_res = _lip_resDefault
		SetSliderOptionValue(a_option, _lip_res, "{1}")

	elseif (a_option == _slider_lip_int)
		_lip_int = _lip_intDefault
		SetSliderOptionValue(a_option, _lip_int, "{1}")

	elseif (a_option == _menu_audio_mode)
		_audio_mode = _audio_modeDefault
		ApplyAudioMode()
		SetMenuOptionValue(a_option, AudioModeLabel(_audio_mode))
		_prismaMcmRevision += 1
		PublishPrismaMCMState()
		; Redraw so the mode-dependent rows pick up their new enabled state.
		ForcePageReset()

	elseif (a_option == _slider_curve_legacy_distance)
		_curve_legacy_distance = 1.0
		controlScript.setConf("_curve_legacy_distance", _curve_legacy_distance)
		SetSliderOptionValue(a_option, _curve_legacy_distance, "{1}")

	elseif (a_option == _toggleCameraBasedAudio)
		_camera_based_audio_state = _camera_based_audio_stateDefault
		if (_camera_based_audio_state)
			controlScript.setConf("_camera_based_audio", 1)
		else
			controlScript.setConf("_camera_based_audio", 0)
		endif
		SetToggleOptionValue(a_option, _camera_based_audio_state)

	elseif (a_option == _toggleInvertHeading)
		_invertheadingstate = _invertheadingstateDefault
		SetToggleOptionValue(a_option, _invertheadingstate)

	elseif (a_option == _togglePauseDialogue)
		_pauseDialogueState = _pauseDialogueStateDefault
		SetToggleOptionValue(a_option, _pauseDialogueState)

	elseif (a_option == _togglePlayerTtsTraditionalDialogue)
		_playerTtsTraditionalDialogueState = _playerTtsTraditionalDialogueStateDefault
		controlScript.setConf("_player_tts_traditional_dialogue", 0)
		SetToggleOptionValue(a_option, _playerTtsTraditionalDialogueState)

	elseif (a_option == _toggleCaptureBackgroundChat)
		_captureBackgroundChatState = _captureBackgroundChatStateDefault
		controlScript.setConf("_capture_background_chat", 1)
		SetToggleOptionValue(a_option, _captureBackgroundChatState)
		
	elseif (a_option == _toggle_openmic)
		_toggle_openmic_state = _toggle_openmic_stateDefault
		SetToggleOptionValue(a_option, _toggle_openmic_state)
		
	elseif (a_option == _slider_openmic_sensitivity)
		_openmic_sensitivity = _openmic_sensitivityDefault
		SetSliderOptionValue(a_option, _openmic_sensitivity, "{0}")
		
	elseif (a_option == _slider_openmic_enddelay)
		_openmic_enddelay = _openmic_enddelayDefault
		SetSliderOptionValue(a_option, _openmic_enddelay, "{1}")
		
	elseif (a_option == _keymap_openmic_mute)
		controlScript.removeBinding(_openmic_mute_key)
		_openmic_mute_key = _openmic_mute_keyDefault
		SetKeymapOptionValue(a_option, _openmic_mute_key)
		controlScript.doBinding9(_openmic_mute_key)

	elseif (a_option == _toggle_autoadd_creature_npcs)
		_toggle_autoadd_creature_npcs_state = false
		controlScript.setConf("_autoadd_creature_npcs", 0)
		SetToggleOptionValue(a_option, _toggle_autoadd_creature_npcs_state)

	endIf
	
endEvent

event OnOptionKeyMapChange(int a_option, int a_keyCode, string a_conflictControl, string a_conflictName)
	{Called when a key has been remapped}

	bool continue = true
	if (a_conflictControl != "" && a_keyCode != 1)
		string msg
		if (a_conflictName != "")
			msg = "This key is already mapped to:\n'" + a_conflictControl + "'\n(" + a_conflictName + ")\n\nAre you sure you want to continue?"
		else
			msg = "This key is already mapped to:\n'" + a_conflictControl + "'\n\nAre you sure you want to continue?"
		endIf

		continue = ShowMessage(msg, true, "$Yes", "$No")
	endIf

	; clear if escape key
	if (a_keyCode == 1)
		a_keyCode = -1
	endIf

	bool prismaChatHotkeyConflict = a_keyCode != -1 && ((a_option == _keymap_chatbox && a_keyCode == _chatbox_focus_key) || (a_option == _keymap_chatbox_focus && a_keyCode == _chatbox_key))
	if (prismaChatHotkeyConflict)
		ShowMessage("Text Chat and Chatbox View must use different hotkeys.")
		return
	endIf

	if (continue)
		if (a_option == _keymapOID_K)
			controlScript.removeBinding(_myKey)
			_myKey = a_keyCode
			controlScript.doBinding(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif

		elseif (a_option == _keymapOID_K2)
			controlScript.removeBinding(_myKey2)
			_myKey2 = a_keyCode
			controlScript.doBinding2(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif

		elseif (a_option == _keymapOID_K3)
			controlScript.removeBinding(_myKey3)
			_myKey3 = a_keyCode
			controlScript.doBinding3(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif

		elseif (a_option == _keymapOID_K4)
			controlScript.removeBinding(_myKey4)
			_myKey4 = a_keyCode
			controlScript.doBinding4(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif

		elseif (a_option == _keymapOID_K5)
			controlScript.removeBinding(_myKey5)
			_myKey5 = a_keyCode
			controlScript.doBinding5(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif

		elseif (a_option == _keymapOID_K6)
			controlScript.removeBinding(_myKey6)
			_myKey6 = a_keyCode
			controlScript.doBinding6(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif

		elseif (a_option == _keymapOID_K7)
			controlScript.removeBinding(_myKey7)
			_myKey7 = a_keyCode
			controlScript.doBinding7(a_keyCode)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_soulgaze)
			controlScript.removeBinding(_soulgaze_key)
			_soulgaze_key = a_keyCode
			controlScript.doBinding20(_soulgaze_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_godmode)
			controlScript.removeBinding(_godmode_key)
			_godmode_key = a_keyCode
			controlScript.doBinding8(_godmode_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif	
		elseif (a_option == _keymap_openmic_mute)
			controlScript.removeBinding(_openmic_mute_key)
			_openmic_mute_key = a_keyCode
			controlScript.doBinding9(_openmic_mute_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_halt)
			controlScript.removeBinding(_halt_key)
			_halt_key = a_keyCode
			controlScript.doBinding10(_halt_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_masterwheel)
			controlScript.removeBinding(_masterwheel_key)
			_masterwheel_key = a_keyCode
			controlScript.doBinding11(_masterwheel_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_overlaystatus_cycle)
			controlScript.removeBinding(_overlaystatus_cycle_key)
			_overlaystatus_cycle_key = a_keyCode
			controlScript.doBinding12(_overlaystatus_cycle_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_historydiaries_cycle)
			controlScript.removeBinding(_historydiaries_cycle_key)
			_historydiaries_cycle_key = a_keyCode
			controlScript.doBinding13(_historydiaries_cycle_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_browser)
			controlScript.removeBinding(_browser_key)
			_browser_key = a_keyCode
			controlScript.doBinding14(_browser_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_debugger)
			controlScript.removeBinding(_debugger_key)
			_debugger_key = a_keyCode
			controlScript.doBinding15(_debugger_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_chatbox)
			controlScript.removeBinding(_chatbox_key)
			_chatbox_key = a_keyCode
			controlScript.doBinding16(_chatbox_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_chatbox_focus)
			controlScript.removeBinding(_chatbox_focus_key)
			_chatbox_focus_key = a_keyCode
			controlScript.doBinding17(_chatbox_focus_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_settingsmenu)
			controlScript.removeBinding(_settingsmenu_key)
			_settingsmenu_key = a_keyCode
			controlScript.doBinding18(_settingsmenu_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		elseif (a_option == _keymap_mastermenu)
			controlScript.removeBinding(_mastermenu_key)
			_mastermenu_key = a_keyCode
			controlScript.doBinding19(_mastermenu_key)
			if (a_keyCode == -1)
				ForcePageReset()
			else
				SetKeymapOptionValue(a_option, a_keyCode)
			endif
		endIf
		
	endIf
endEvent

event OnOptionSelect(int a_option)
	{Called when the user selects a non-dialog option}
	
	if (a_option == _toggle1OID_B)
		_toggleState1 = !_toggleState1
		if (_toggleState1)
			;controlScript.setTTSOn();
		else
			;controlScript.setTTSOff();		
		endIf
		SetToggleOptionValue(a_option, _toggleState1)
	endIf
	if (a_option == _toggle1OID_C)
		_toggleState2 = !_toggleState2
		if (_toggleState2)
			controlScript.setNewActionMode(1);	
		else
			controlScript.setNewActionMode(0);
		endif		
		SetToggleOptionValue(a_option, _toggleState2)
	endIf
	if (a_option == _toggle1OID_D)
		_toggleState3 = !_toggleState3
		if (_toggleState3)
			controlScript.setVoiceType(1);	
		else
			controlScript.setVoiceType(0);
		endif		
		SetToggleOptionValue(a_option, _toggleState3)
	endIf
	if (a_option == _toggle1OID_E)
		_toggleState7 = !_toggleState7
		if (_toggleState7)
			controlScript.setSoulgazeModeNative(1)
		else
			controlScript.setSoulgazeModeNative(0)
		endif		
		SetToggleOptionValue(a_option, _toggleState7)
	endIf
	if (a_option == _toggleAnimation)
		_animationstate = !_animationstate
		
		if (_animationstate)
			bool r=controlScript.setConf("_animations",1)
		else
			bool r=controlScript.setConf("_animations",0)
		endif
		
		SetToggleOptionValue(a_option, _animationstate)
	endIf
	if (a_option == _toggle1OID_Rereg)
		ConsoleUtil.ExecuteCommand("SetStage SKI_ConfigManagerInstance 1")
		ShowMessage("Close menu")
	endIf

	if (a_option == _toggleCameraBasedAudio)
		_camera_based_audio_state = !_camera_based_audio_state
		
		if (_camera_based_audio_state)
			controlScript.setConf("_camera_based_audio",1)
		else
			controlScript.setConf("_camera_based_audio",0)
		endif
		
		SetToggleOptionValue(a_option, _camera_based_audio_state)
	endIf

	if (a_option == _toggleInvertHeading)
		_invertheadingstate = !_invertheadingstate
		
		if (_invertheadingstate)
			controlScript.setConf("_invertheadingstate",1)
		else
			controlScript.setConf("_invertheadingstate",0)
		endif
		
		SetToggleOptionValue(a_option, _invertheadingstate)
	endIf

	if (a_option == _togglePauseDialogue)
		_pauseDialogueState = !_pauseDialogueState
		
		if (_pauseDialogueState)
			controlScript.setConf("_pause_dialogue_when_menu_open",1)
		else
			controlScript.setConf("_pause_dialogue_when_menu_open",0)
		endif
		
		SetToggleOptionValue(a_option, _pauseDialogueState)
	endIf

	if (a_option == _togglePlayerTtsTraditionalDialogue)
		_playerTtsTraditionalDialogueState = !_playerTtsTraditionalDialogueState
		
		if (_playerTtsTraditionalDialogueState)
			controlScript.setConf("_player_tts_traditional_dialogue",1)
		else
			controlScript.setConf("_player_tts_traditional_dialogue",0)
		endif
		
		SetToggleOptionValue(a_option, _playerTtsTraditionalDialogueState)
	endIf

	if (a_option == _toggleCaptureBackgroundChat)
		_captureBackgroundChatState = !_captureBackgroundChatState

		if (_captureBackgroundChatState)
			controlScript.setConf("_capture_background_chat",1)
		else
			controlScript.setConf("_capture_background_chat",0)
		endif

		SetToggleOptionValue(a_option, _captureBackgroundChatState)
	endIf
	
	if (a_option == _toggle_npc_go_near)
		_toggle_npc_go_near_state = !_toggle_npc_go_near_state
		if (_toggle_npc_go_near_state)
			StorageUtil.SetIntValue(None, "AIAgentNpcWalkNear",1);
		else
			StorageUtil.SetIntValue(None, "AIAgentNpcWalkNear",0);
		endif
		SetToggleOptionValue(a_option, _toggle_npc_go_near_state)

	endif
	
	if (a_option == _toggle_npc_walk_to_target)
		_toggle_npc_walk_to_target_state = !_toggle_npc_walk_to_target_state
		if (_toggle_npc_walk_to_target_state)
			StorageUtil.SetIntValue(None, "AIAgentNpcWalkToTarget",1);
		else
			StorageUtil.SetIntValue(None, "AIAgentNpcWalkToTarget",0);
		endif
		SetToggleOptionValue(a_option, _toggle_npc_walk_to_target_state)

	endif
	
	if (a_option == _toggle_autofocus_on_sit)
		_toggle_autofocus_on_sit_state = !_toggle_autofocus_on_sit_state
		if (_toggle_autofocus_on_sit_state)
			StorageUtil.SetIntValue(None, "AIAgentAutoFocusOnSit",1);
		else
			StorageUtil.SetIntValue(None, "AIAgentAutoFocusOnSit",0);
		endif
		SetToggleOptionValue(a_option, _toggle_autofocus_on_sit_state)

	endif
	
	if (a_option == _toggleAddAllNPC)
 		_toggleAddAllNPCState = !_toggleAddAllNPCState
 
 		if (_toggleAddAllNPCState)
 			controlScript.setConf("_toggleAddAllNPC",1)
 		else
 			controlScript.setConf("_toggleAddAllNPC",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggleAddAllNPCState)
 	endIf
	

	
	
	if (a_option == _toggle_restrict_onscene)
 		_toggle_restrict_onscene_state = !_toggle_restrict_onscene_state
 
 		if (_toggle_restrict_onscene_state)
 			controlScript.setConf("_restrict_onscene",1)
 		else
 			controlScript.setConf("_restrict_onscene",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_restrict_onscene_state)
 	endIf
	
	if (a_option == _toggle_usewebsocketstt)
 		_toggle_usewebsocketstt_state = !_toggle_usewebsocketstt_state
 
 		if (_toggle_usewebsocketstt_state)
			StorageUtil.SetIntValue(None, "AIAgentWebSockeSTT",1);
 		else
 			StorageUtil.SetIntValue(None, "AIAgentWebSockeSTT",0);
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_usewebsocketstt_state)
 	endIf
	
	if (a_option == _toggle_autoadd_hostile)
 		_toggle_autoadd_hostile_state = !_toggle_autoadd_hostile_state
 
 		if (_toggle_autoadd_hostile_state)
 			controlScript.setConf("_autoadd_hostile",1)
 		else
 			controlScript.setConf("_autoadd_hostile",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_autoadd_hostile_state)
 	endIf
	
	if (a_option == _toggle_autoadd_allraces)
 		_toggle_autoadd_allraces_state = !_toggle_autoadd_allraces_state
 
 		if (_toggle_autoadd_allraces_state)
 			controlScript.setConf("_autoadd_allraces",1)
 		else
 			controlScript.setConf("_autoadd_allraces",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_autoadd_allraces_state)
 	endIf
	
	if (a_option == _toggle_autoadd_creature_npcs)
		_toggle_autoadd_creature_npcs_state = !_toggle_autoadd_creature_npcs_state

		if (_toggle_autoadd_creature_npcs_state)
			controlScript.setConf("_autoadd_creature_npcs",1)
		else
			controlScript.setConf("_autoadd_creature_npcs",0)
		endif

		SetToggleOptionValue(a_option, _toggle_autoadd_creature_npcs_state)
	endIf

	if (a_option == _toggle_openmic)
 		_toggle_openmic_state = !_toggle_openmic_state
 
 		if (_toggle_openmic_state)
 			controlScript.setConf("_openmic_enabled",1)
 		else
 			controlScript.setConf("_openmic_enabled",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_openmic_state)
 	endIf
	
	
	if (a_option == _toggle_combatdialogue)
 		_toggle_combatdialogue_state = !_toggle_combatdialogue_state
 
 		if (_toggle_combatdialogue_state)
 			controlScript.setConf("_combat_dialogue",1)
 		else
 			controlScript.setConf("_combat_dialogue",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_combatdialogue_state)
 	endIf
	
	if (a_option == _toggle_cancel_dialogue_on_combat)
 		_toggle_cancel_dialogue_on_combat_state = !_toggle_cancel_dialogue_on_combat_state
 
 		if (_toggle_cancel_dialogue_on_combat_state)
 			controlScript.setConf("_cancel_dialogue_on_combat",1)
 		else
 			controlScript.setConf("_cancel_dialogue_on_combat",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_cancel_dialogue_on_combat_state)
 	endIf
	
	if (a_option == _toggle_combat_barks)
 		_toggle_combat_barks_state = !_toggle_combat_barks_state
 
 		if (_toggle_combat_barks_state)
 			controlScript.setConf("_combat_barks",1)
 		else
 			controlScript.setConf("_combat_barks",0)
 		endif
 
 		SetToggleOptionValue(a_option, _toggle_combat_barks_state)
 	endIf
	
	if (a_option == _actionSendLocations)
		ShowMessage("Please wait 3-5 minutes, stay at this screen waiting for end confirmation. You can bring up console to check progress ")
 		AIAgentPapyrusFunctions.RunToolsSendFactionLocationInfo()
 		ShowMessage("factions,locations and unique NPCs fully synced and complete!")
 	endIf
	
  	if (a_option == _actionSendVoices)
		ShowMessage("Uploading all voice samples. Will take 20-30 seconds.")
		int voiceUploadResult = AIAgentPapyrusFunctions.RunToolsSendAllVoiceSamples()
		if (voiceUploadResult == 0)
			ShowMessage("Voice samples uploaded successfully")
		endif
	endIf
	
 	; Handle AI Agents page options
 	if (a_option == _toggleAddAllNowNPC)
 		AIAgentFunctions.testAddAllNPCAround()
 		ForcePageReset()
 		ShowMessage("AI agents added successfully")
 	endIf
 	
 	if (a_option == _removeAllAgentsOID)
 		bool confirmed = ShowMessage("Are you sure you want to remove ALL AI agents? Won't do anything if Auto Activate is enabled.", true, "$Yes", "$No")
 		if (confirmed)
 			AIAgentFunctions.testRemoveAll()
 			ForcePageReset()
 			ShowMessage("All AI agents removed")
 		endif
 	endIf
 	
 	; Handle individual agent removal
 	if (_agentToggleOIDs && _currentAgentNames)
 		int i = 0
 		while i < _agentToggleOIDs.Length
 			if (a_option == _agentToggleOIDs[i] && _currentAgentNames[i] != "")
 				bool confirmed = ShowMessage("Remove AI agent '" + _currentAgentNames[i] + "'?", true, "$Yes", "$No")
 				if (confirmed)
 					AIAgentFunctions.removeAgentByName(_currentAgentNames[i])
 					ForcePageReset()
 					ShowMessage("Removed AI agent: " + _currentAgentNames[i])
 				endif
 				return
 			endif
 			i += 1
 		endwhile
 	endif
 	
 	; Handle refresh nearby NPCs
 	if (a_option == _refreshNearbyNPCsOID)
 		ForcePageReset()
 		ShowMessage("Nearby NPCs list refreshed")
 	endIf
 	
 	; Handle individual nearby NPC addition
 	if (_nearbyNpcToggleOIDs && _nearbyNpcNames)
 		int k = 0
 		while k < _nearbyNpcToggleOIDs.Length
 			if (a_option == _nearbyNpcToggleOIDs[k] && _nearbyNpcNames[k] != "")
 				bool confirmed = ShowMessage("Add AI agent '" + _nearbyNpcNames[k] + "'?", true, "$Yes", "$No")
 				if (confirmed)
 					Actor targetNPC = None
 					
 					; Find the actual actor by name from the nearby non-agents list
 					Actor[] nearbyNonAgents = AIAgentFunctions.findAllNearbyNonAgents()
 					int m = 0
 					while m < nearbyNonAgents.Length && !targetNPC
 						if (nearbyNonAgents[m] && nearbyNonAgents[m].GetDisplayName() == _nearbyNpcNames[k])
 							targetNPC = nearbyNonAgents[m]
 						endif
 						m += 1
 					endwhile
 					
 					if (targetNPC)
 						AIAgentFunctions.setDrivenByAIA(targetNPC, true)
 						ForcePageReset()
 						ShowMessage("Added AI agent: " + _nearbyNpcNames[k])
 					else
 						ShowMessage("Could not find NPC: " + _nearbyNpcNames[k])
 					endif
 				endif
 				return
 			endif
 			k += 1
 		endwhile
 	endif
	
endEvent

event OnOptionHighlight(int a_option)
	{Called when the user highlights an option}
	
	if (a_option == _keymapOID_K)
		SetInfoText("Deprecated text chat input. Tap to type a message. Hold to make the NPC in your crosshair wait here. Use Text Chat for Prisma UI.")
	endIf
	if (a_option == _toggle1OID_B)
		SetInfoText("Enables Text-to-Speech for AI NPCs.")
	endIf
	if (a_option == _keymapOID_K2)
		SetInfoText("Hold to talk. Tap to stop current and queued dialogue without halting NPC actions. Double-tap to make the NPC in your crosshair wait here. With a book open, press to summarize it. CHIM uses the Windows default recording device shown below.")
	endIf
	if (a_option == _toggle1OID_C)
		SetInfoText("Enable AI to perform actions.")
	endIf
	if (a_option == _toggle1OID_D)
		SetInfoText("If using mods like RDO, check this to force default voice, so dialog Follow me should appear. Note that checking this will disable custom voiced sounds. As of version 0.9.x, this shouldn't be needed.")
	endIf
	if (a_option == _keymapOID_K3)
		SetInfoText("Settings Wheel - Looking at NPC: Assign profiles (1-4). Not looking: Switch LLM models, toggle Compact Chat.")
	endIf
	if (a_option == _keymapOID_K4)
		SetInfoText("Roleplay Wheel - Write Diary, Gather NPCs, Update NPC, Wait/Follow, Stop All AI, Add to BgL. Hold it for nearby NPCs to write diary entries.")
	endIf
	if (a_option == _keymapOID_K5)
		SetInfoText("Change AI/LLM Connector.")
	endIf
	if (a_option == _keymapOID_K6)
		SetInfoText("Soulgaze Wheel - Take screenshots for ITT: Full soulgaze, NPC photos (zoomed/standard), or raw upload.")
	endIf
	if (a_option == _keymapOID_K7)
		SetInfoText("Manually activate/deactivate AI control for the targeted NPC or all nearby NPCs if none targeted.")
	endIf
	if (a_option == _slider_volume)
		SetInfoText("Set AI NPC speech volume.")
	endIf
	if (a_option == _slider_head_voice_volume)
		SetInfoText("Adjust narrator and player TTS playback relative to AI Voice Volume. 100% keeps the current level; 0% mutes both in-head voices only.")
	endIf
	if (a_option == _slider_preclip)
		SetInfoText("Skips specified millisecods at begining of a sentence. Some TTS services add some silence at the begining of audio clips.")
	endIf
	if (a_option == _slider_postclip)
		SetInfoText("Skips specified millisecods at end of a sentence. Some TTS services add some silence at the end of audio clips.")
	endIf
	if (a_option == _slider_ds)
		SetInfoText("Adjust AI NPC volume at distance. Range: 0.1 to 20.0. Used by 3D Advanced and Mono + Advanced Effects only.")
	endIf
	if (a_option == _slider_playback_dropoff_inside)
		SetInfoText("Indoor playback dropoff aggressiveness. 100 = current behavior. Lower values are less aggressive (default 70). Used by 3D Advanced and Mono + Advanced Effects only.")
	endIf
	if (a_option == _slider_playback_dropoff_outside)
		SetInfoText("Outdoor playback dropoff aggressiveness. 100 = current behavior. Lower values are less aggressive (default 70). Used by 3D Advanced and Mono + Advanced Effects only.")
	endIf
	if (a_option == _menu_audio_mode)
		SetInfoText("3D Advanced: directional with distance fading and muffling. 3D Legacy: directional with legacy distance fading. 2D Flat: directional without distance fading. Mono: non-positional. Mono + Advanced Effects: non-positional with advanced distance fading and muffling. Dialogue awareness is unaffected.")
	endIf
	if (a_option == _toggleCameraBasedAudio)
		SetInfoText("When enabled, 3D voice direction follows the camera facing instead of the player actor heading. Off by default. Used by 2D Flat, 3D Legacy and 3D Advanced only.")
	endIf
	if (a_option == _toggle1OID_E)
		SetInfoText("Enable HD mode for Soulgaze (DirectX backbuffer access, server compression). Disable for in-game screenshots (VR users should disable).")
	endIf
	
	if (a_option == _slider_lip_int)
		SetInfoText("Lip modifier intensity. Set it lower if mouth opens too much ")
	endIf
	
	if (a_option == _slider_lip_res)
		SetInfoText("Lip modifier resolution. Set it lower if movement is too laggy. Lower uses more CPU. Find your sweet spot.")
	endIf
	
	if (a_option == _slider_timeout)
		SetInfoText("Connection timeout when requesting data from CHIM Server. Recommended: 60 seconds.")
	endIf
	
	if (a_option == _toggleAnimation)
		SetInfoText("Enable AI NPCs to perform animations during interactions.")
	endIf
	
	if (a_option == _toggle1OID_Rereg)
		SetInfoText("Mod name has changed. This will reset MCM to show new name. May affect other mods. Will call setstage SKI_ConfigManagerInstance 1")
	endIf
	
	if (a_option == _toggleInvertHeading)
		SetInfoText("Inverts the 3D audio heading. This may resolve issues where NPCs in the front are heard at a lower volume. Used by 2D Flat, 3D Legacy and 3D Advanced only.")
	endIf

	if (a_option == _togglePauseDialogue)
		SetInfoText("Enable to pause dialogue during game pauses. Disable to allow dialogue to continue during game pauses.")
	endIf

	if (a_option == _togglePlayerTtsTraditionalDialogue)
		SetInfoText("Will play whatever PlayerTTS is selected for traditional dialogue. It must be enabled and set within the CHIM webpage and requires the optional regular or VR dialogue menu interface patch.")
	endIf

	if (a_option == _toggleCaptureBackgroundChat)
		SetInfoText("When on, vanilla dialogue-menu conversations and nearby ambient NPC chatter are added to AI context. When off, neither is captured; normal dialogue and subtitles still work.")
	endIf

	if (a_option == _slider_max_distance_inside)
		SetInfoText("AI within this distance in interiors are Auto Activated.")
	endIf
	
	if (a_option == _slider_max_distance_outside)
		SetInfoText("AI within this distance outside are Auto Activated.")
	endIf

	if (a_option == _slider_spatial_hearing_inside)
		SetInfoText("Sets indoor conversation hearing distance for spatial awareness checks.")
	endIf

	if (a_option == _slider_spatial_hearing_outside)
		SetInfoText("Sets outdoor conversation hearing distance for spatial awareness checks.")
	endIf

	if (a_option == _slider_auto_hearing_radius_m)
		SetInfoText("Direct auto hearing radius in meters. Uses straight-line distance and does not require LOS or navmesh.")
	endIf
	
	if (a_option == _toggleAddAllNPC)
		SetInfoText("Will Auto Activate (almost) force all current NPCs.")
	endIf
	
	if (a_option == _slider_bored_period)
		SetInfoText("How many seconds (with some exceptions) a Bored event can potenitally be triggered.")
	endIf
	
	if (a_option == _slider_dynamic_profile_period)
		SetInfoText("Timer for automatic dynamic profile updates. Updates NPC personalities based on recent events.")
	endIf
	
	if (a_option == _toggle_npc_go_near)
		SetInfoText("When enabled NPC's will subtly move around the player to make listening to conversations easier.")
	endIf
	
	if (a_option == _toggle_npc_walk_to_target)
		SetInfoText("NPCs will walk towards the NPC they are talking to. May break scenes - use Scene Safety with this option.")
	endIf
	
	if (a_option == _toggle_autofocus_on_sit)
		SetInfoText("Only works when player is seated and 1st person. Automatically rotate the camrea to a talking NPC. It's like Netflix!")
	endIf
	
	if (a_option == _toggle_restrict_onscene)
		SetInfoText("Prevent AI NPCs in a traditional dialogue scene from responding automatically.")
	endIf
	
	if (a_option == _toggle_usewebsocketstt)
		SetInfoText("Use WebSocket STT. Overrides CHIM server STT. Must download separately from the mod page. WIP.")
	endIf
	
	if (a_option == _keymap_godmode)
		SetInfoText("Mode Wheel - Switch between chat modes: Standard, Whisper, Director, Cheat Mode, Auto Chat, Inject. Hold to cycle modes.")
	endIf
	
	if (a_option == _keymap_halt)
		SetInfoText("Immediately stop all CHIM AI actions for targeted NPC or all nearby NPCs.")
	endIf
	
	if (a_option == _keymap_masterwheel)
		SetInfoText("Master Wheel - Quick access menu to open any of the 4 wheels: Roleplay, Settings, Mode, or Soulgaze.")
	endIf
	
	if (a_option == _keymap_historydiaries_cycle)
		SetInfoText("Cycle through reading panels: Press once for Conversation History, press again for Diaries, press again to close both. Both pause the game. Requires Prisma UI.")
	endIf
	
	if (a_option == _keymap_overlaystatus_cycle)
		SetInfoText("Cycle through the Prisma status views: Status, Minihud, and Terminator. Press again to close. Requires Prisma UI.")
	endIf
	
	if (a_option == _keymap_browser)
		SetInfoText("Toggle the CHIM Browser (Beta). Browse the full HerikaServer web interface in-game with cursor and typing support. Game pauses when open. Press the hotkey again to close. Requires Prisma UI.")
	endIf
	
	if (a_option == _keymap_debugger)
		SetInfoText("Open the CHIM Logs View (Beta). Access server logs, settings, and diagnostics from the HerikaServer control panel. Press hotkey again to close. Requires Prisma UI.")
	endIf
	
	if (a_option == _keymap_chatbox)
		SetInfoText("Open Chatbox View in Prisma UI. This is the MMO-style live chat panel that stays open while dialogue and system messages stream in. Requires Prisma UI.")
	endIf
	
	if (a_option == _keymap_chatbox_focus)
		SetInfoText("Open Text Chat immediately in Prisma UI. Holding the key also opens it. With a book open, press to summarize it.")
	endIf
	
	if (a_option == _keymap_settingsmenu)
		SetInfoText("Open the Actions Menu. This is the Prisma UI action panel for in-game AI interactions and commands. Requires Prisma UI.")
	endIf
	
	if (a_option == _keymap_mastermenu)
		SetInfoText("Open the CHIM Master Menu. Quick launcher for all Prisma UI panels. Game pauses when open. Select a panel to toggle it. Requires Prisma UI.")
	endIf

	if (a_option == _keymap_soulgaze)
		SetInfoText("Tap to capture visual context without speech. Double-tap while aiming at an activated AI NPC to update their portrait. Hold to ask the nearest activated NPC to describe the scene.")
	endIf
	
	if (a_option == _toggle_autoadd_hostile)
		SetInfoText("Auto Activate policy. By default, it applies to non-hostile NPCs whose race allows player dialogue (PC Dialogue = 1). Check this to allow Auto Activate hostile NPCs")
	endIf
	
	if (a_option == _toggle_autoadd_allraces)
		SetInfoText("Auto Activate policy. By default, it applies to non-hostile NPCs whose race allows player dialogue (PC Dialogue = 1). Check this option to allow Auto Activate for all races - including animals like rabbits, deer, foxes, etc. Note: Enabling this may cause instability.")
	endIf
	
	if (a_option == _toggle_autoadd_creature_npcs)
		SetInfoText("Auto Activate policy. Adds a set group of creatures: dragons, hagravens, giants, Falmer, spriggans, werewolves, undead, dwarven automatons and animal followers. Ordinary wildlife and unrecognized modded creatures stay excluded. Hostile ones are only added if Add Hostile NPCs is also on. Add All races still overrides this.")
	endIf

	if (a_option == _actionSendLocations)
		SetInfoText("Send faction,location and unique NPCs info to the server. This can take 3-5 minutes and only needs to be done once per playthrough.")
	endIf
	
	if (a_option == _actionSendVoices)
		SetInfoText("Upload all available voice type samples to the server.")
	endIf

	if (a_option == _toggle_openmic)
		SetInfoText("Enable open microphone mode. Will automatically start recording when it detects voice input above the sensitivity threshold.")
	endIf
	
	if (a_option == _slider_openmic_sensitivity)
		SetInfoText("Voice detection sensitivity for open mic. Higher values require louder voice to trigger recording.")
	endIf
	
	if (a_option == _slider_openmic_enddelay)
		SetInfoText("How long to wait (in seconds) after voice stops before ending the recording and processing the speech.")
	endIf
	
	if (a_option == _keymap_openmic_mute)
		SetInfoText("Key to temporarily mute open microphone.")
	endIf
	if (a_option == _text_current_recording_device)
		SetInfoText("Read-only display of the Windows recording device currently resolved by CHIM's voice capture path.")
	endIf
	

	if (a_option == _toggle_combatdialogue)
		SetInfoText("Enable combat dialogue.")
	endIf
	
	if (a_option == _toggle_cancel_dialogue_on_combat)
		SetInfoText("When enabled, all AI dialogue will be immediately cancelled when you enter combat (prevents NPCs talking during fights)")
	endIf
	
	if (a_option == _toggle_combat_barks)
		SetInfoText("When enabled AI agents in combat will periodically shout taunts/battle cries. Is controlled via Rechat. ")
	endIf
	
	if (a_option == _slider_combat_barks_period)
		SetInfoText("How often (in seconds) combat barks trigger during active combat. Will automatically trigger an event when combat starts. Default: 30 seconds")
	endIf

	; AI Agents page help text
	if (a_option == _toggleAddAllNowNPC)
		SetInfoText("Will Auto Activate (almost) all nearby NPCs.")
	endIf
	
	if (a_option == _removeAllAgentsOID)
		SetInfoText("Remove all active AI agents from the system.")
	endIf

	if (a_option == _slider_curve_legacy_distance)
		SetInfoText("Curve distance scale for the emitter. How much actors are attenuated based on distance. Higher values: less attenuation. Lower values: more attenuation. Values below 1 select 2D Flat, retaining direction without distance fading. Used by 3D Legacy only.")
	endIf

	if (a_option == _slider_maintenance_period)
		SetInfoText("How often run maintenance (restore voices, delete unussed agents). In seconds")
	endIf

	; Help text for individual agent removal options
	if (_agentToggleOIDs && _currentAgentNames)
		int i = 0
		while i < _agentToggleOIDs.Length
			if (a_option == _agentToggleOIDs[i] && _currentAgentNames[i] != "")
				SetInfoText("Remove the AI agent '" + _currentAgentNames[i] + "' from the active AI system.")
				return
			endif
			i += 1
		endwhile
	endif

	; Help text for refresh nearby NPCs
	if (a_option == _refreshNearbyNPCsOID)
		SetInfoText("Refresh the list of nearby NPCs that can be added to the AI system.")
	endIf
	
	; Help text for individual nearby NPC addition
	if (_nearbyNpcToggleOIDs && _nearbyNpcNames)
		int j = 0
		while j < _nearbyNpcToggleOIDs.Length
			if (a_option == _nearbyNpcToggleOIDs[j] && _nearbyNpcNames[j] != "")
				SetInfoText("Add the nearby NPC '" + _nearbyNpcNames[j] + "' to the AI system.")
				return
			endif
			j += 1
		endwhile
	endif


endEvent
