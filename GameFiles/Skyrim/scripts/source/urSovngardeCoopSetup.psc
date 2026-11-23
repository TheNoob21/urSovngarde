ScriptName urSovngardeCoopSetup extends Quest
; On the start-game-enabled quest of urSovngardeCoopMenu.esp: hands the player the power, in a new game and in a save made before the mod.

Event OnInit()
    GivePower()
EndEvent

; Safe to call again after a load.
Function GivePower() global
    Spell power = Game.GetFormFromFile(0x00000801, "urSovngardeCoopMenu.esp") as Spell
    Actor player = Game.GetPlayer()
    If power && player && !player.HasSpell(power)
        player.AddSpell(power, false)
    EndIf
EndFunction
