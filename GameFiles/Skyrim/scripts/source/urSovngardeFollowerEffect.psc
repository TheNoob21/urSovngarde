ScriptName urSovngardeFollowerEffect extends ActiveMagicEffect
; The effect of the self spell urSovngardeFollowerSPEL (urSovngardeCoopMenu.esp, MGEF 0x805, SPEL 0x806), cast by the urSovngarde client:
; - on this player, when they activate another player's follower: the choice of a word to her, or a trade (MESG 0x807);
; - on a follower this game runs, when another player gave her a word: the game's own follower script carries it out,
;   so the follower quest stays in step (a follower left waiting is still dismissed after three days, as alone).

Event OnEffectStart(Actor akTarget, Actor akCaster)
    If akTarget == Game.GetPlayer()
        Choose()
    Else
        CarryOut(akTarget)
    EndIf
EndEvent

Function Choose()
    Message menu = Game.GetFormFromFile(0x00000807, "urSovngardeCoopMenu.esp") as Message
    If !menu
        Return
    EndIf
    ; Buttons: 0 Wait here, 1 Follow her player, 2 Trade, 3 Cancel.
    Actor follower = Game.GetFormEx(urSovngardeCoop.ChosenFollower()) as Actor
    int choice = menu.Show()
    If choice == 0
        urSovngardeCoop.AskFollower(1)
    ElseIf choice == 1
        urSovngardeCoop.AskFollower(2)
    Else
        ; Trade asks nothing of her player's game: the items moved here reach it as any other change of her inventory.
        urSovngardeCoop.AskFollower(0)
        If choice == 2 && follower
            Trade(follower)
        EndIf
    EndIf
EndFunction

; The game's own follower trade, on her copy here: as this player's teammate for as long as the menu is open, so taking is not stealing.
; Her gear stays her player's game's: an item equipped here is not sent, and she wears what her own game gives her.
Function Trade(Actor follower)
    bool wasTeammate = follower.IsPlayerTeammate()
    If !wasTeammate
        follower.SetPlayerTeammate(true, false)
    EndIf
    follower.OpenInventory(true)
    ; A wait returns only once the game leaves menu mode, so this runs after the menu closed.
    Utility.Wait(0.1)
    If !wasTeammate
        follower.SetPlayerTeammate(false, false)
    EndIf
EndFunction

Function CarryOut(Actor follower)
    int formId = follower.GetFormID()
    int command = urSovngardeCoop.TakeFollowerCommand(formId)
    If command == 0
        Return
    EndIf
    ; Replies: 1 done, 2 busy, 3 not the follower of the game's own follower quest.
    If follower.IsOnMount()
        urSovngardeCoop.FollowerCommandDone(formId, command, 2)
        Return
    EndIf
    DialogueFollowerScript followers = Game.GetFormFromFile(0x000750BA, "Skyrim.esm") as DialogueFollowerScript
    If !followers
        urSovngardeCoop.FollowerCommandDone(formId, command, 3)
    ElseIf followers.pFollowerAlias.GetActorReference() == follower
        If command == 1
            followers.FollowerWait()
        Else
            followers.FollowerFollow()
        EndIf
        urSovngardeCoop.FollowerCommandDone(formId, command, 1)
    ElseIf followers.pAnimalAlias.GetActorReference() == follower
        If command == 1
            followers.AnimalWait()
        Else
            followers.AnimalFollow()
        EndIf
        urSovngardeCoop.FollowerCommandDone(formId, command, 1)
    Else
        urSovngardeCoop.FollowerCommandDone(formId, command, 3)
    EndIf
EndFunction
