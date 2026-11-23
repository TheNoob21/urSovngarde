ScriptName SkyrimTogetherVerifyLaunchScript extends Quest  

; Not bound (not implemented) native functions return default values dictated by their return type. 
; `DidLaunchSkyrimTogether()` will return `false` in the base game, however launching 
; the multiplayer launcher will make it return `true` because we bind the function (see Misc/BSScript.cpp)

bool Function DidLaunchSkyrimTogether() global native

Event OnInit()
    VerifyLaunch()
EndEvent

Function VerifyLaunch()
    If (!DidLaunchSkyrimTogether())
        Utility.Wait(1)
        Debug.MessageBox("urSovngarde\n\n" \
                       + "The multiplayer launcher is not running.\n" \
                       + "To play together, " \
                       + "start urSovngardeSE.exe for Special Edition or urSovngarde.exe for VR.")
    EndIf
EndFunction
