ScriptName urSovngardeCoopEffect extends ActiveMagicEffect
; The effect of the lesser power "Call to Sovngarde" (urSovngardeCoopMenu.esp, MGEF 0x800).
; Casting it opens the co-op menu. The power costs nothing, has no cooldown, and a power cannot be dropped.

Event OnEffectStart(Actor akTarget, Actor akCaster)
    If akCaster == Game.GetPlayer()
        ShowMenu()
    EndIf
EndEvent

Function ShowMenu()
    Message menu = Game.GetFormFromFile(0x00000802, "urSovngardeCoopMenu.esp") as Message
    If !menu
        Debug.Notification("urSovngarde: the co-op menu data is missing")
        Return
    EndIf

    int choice = menu.Show()
    If choice == 0
        ShowServers()
    ElseIf choice == 1
        urSovngardeCoop.Disconnect()
    ElseIf choice == 2
        Debug.MessageBox(urSovngardeCoop.GetStatusText())
    ElseIf choice == 4
        urSovngardeCoop.ConnectLastUsed()
    EndIf
EndFunction

Function ShowServers()
    Message picker = Game.GetFormFromFile(0x0000080A, "urSovngardeCoopMenu.esp") as Message
    If !picker
        urSovngardeCoop.Connect()
        Return
    EndIf
    int first = urSovngardeCoop.GetServerId(0)
    If first < 0
        Debug.MessageBox("Could not read the saved server list.")
        Return
    ElseIf first == 0
        urSovngardeCoop.Connect()
        Return
    EndIf

    int[] ids = new int[4]
    int offset = 0
    int lastUsed = urSovngardeCoop.GetLastUsedServer()
    While true
        string text = "Saved servers"
        int slot = 0
        While slot < ids.Length
            ids[slot] = urSovngardeCoop.GetServerId(offset + slot)
            If ids[slot] < 0
                Debug.MessageBox("Could not read the saved server list.")
                Return
            ElseIf ids[slot] > 0
                string name = urSovngardeCoop.GetServerName(ids[slot])
                string address = urSovngardeCoop.GetServerAddress(ids[slot])
                text += "\n" + (slot + 1) + ". " + name
                If name != address
                    text += " (" + address + ")"
                EndIf
                If ids[slot] == lastUsed
                    text += " (last used)"
                EndIf
            EndIf
            slot += 1
        EndWhile
        Debug.MessageBox(text)
        ; Wait for the list to close before opening the controller's numbered choices.
        int waitTicks = 0
        While !UI.IsMenuOpen("MessageBoxMenu") && waitTicks < 10
            Utility.WaitMenuMode(0.1)
            waitTicks += 1
        EndWhile
        While UI.IsMenuOpen("MessageBoxMenu")
            Utility.WaitMenuMode(0.1)
        EndWhile
        int choice = picker.Show()
        If choice >= 0 && choice < ids.Length
            If ids[choice] > 0
                urSovngardeCoop.ConnectSaved(ids[choice])
                Return
            EndIf
        ElseIf choice == 4
            If offset >= ids.Length
                offset -= ids.Length
            EndIf
        ElseIf choice == 5
            If urSovngardeCoop.GetServerId(offset + ids.Length) > 0
                offset += ids.Length
            EndIf
        Else
            Return
        EndIf
    EndWhile
EndFunction
