ScriptName urSovngardeDragonSoulEffect extends ActiveMagicEffect
; The effect of the self spell urSovngardeDragonSoulSPEL (urSovngardeCoopMenu.esp, MGEF 0x808, SPEL 0x809), cast on this player by the
; urSovngarde client when a dragon of the party died (Code/client/DragonSouls.cpp). Every game keeps its dragons in MQNoDragonAbsorb,
; so the game's own absorb gives nothing by itself; here each soul is given once: the game's own absorb if this player came near
; the dragon, as alone, or the soul without it.

Event OnEffectStart(Actor akTarget, Actor akCaster)
    If akTarget != Game.GetPlayer()
        Return
    EndIf
    int soul = urSovngardeCoop.TakeDragonSoul()
    While soul != 0
        If soul > 0
            Absorb(Game.GetForm(soul) as Actor)
        Else
            SoulOnly()
        EndIf
        soul = urSovngardeCoop.TakeDragonSoul()
    EndWhile
EndEvent

; The game's own absorb: its effects, the soul, and the main quest's first dragon.
Function Absorb(Actor dragon)
    Faction noAbsorb = Game.GetFormFromFile(0x0007A0DF, "Skyrim.esm") as Faction
    MQKillDragonScript killDragon = Game.GetFormFromFile(0x00035D56, "Skyrim.esm") as MQKillDragonScript
    If !dragon || !noAbsorb || !killDragon
        SoulOnly()
        Return
    EndIf
    dragon.RemoveFromFaction(noAbsorb)
    killDragon.DeathSequence(dragon, Game.GetPlayer(), False)
EndFunction

; The soul alone, as the absorb gives it (MQKillDragonScript.DeathSequence: VoicePointsReward 1, MQDragonAbsorbCount, MQ104 90).
Function SoulOnly()
    Game.GetPlayer().ModActorValue("DragonSouls", 1)
    GlobalVariable absorbed = Game.GetFormFromFile(0x00035D5D, "Skyrim.esm") as GlobalVariable
    If absorbed
        absorbed.SetValue(absorbed.GetValue() + 1)
    EndIf
    ; The main quest's first dragon (MQ104 stage 90), as the absorb sets it, so a party that was all far from it goes on too.
    Quest mainQuest = Game.GetFormFromFile(0x0002610C, "Skyrim.esm") as Quest
    If mainQuest && !mainQuest.GetStageDone(90) && mainQuest.IsRunning()
        mainQuest.SetStage(90)
    EndIf
    Debug.Notification("Dragon soul absorbed")
EndFunction
