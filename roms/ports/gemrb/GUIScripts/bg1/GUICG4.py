# GemRB - Infinity Engine Emulator
# Copyright (C) 2003 The GemRB Project
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#
#character generation, ability (GUICG4)
import GemRB
from GUIDefines import *
from ie_stats import *
import CharGenCommon
import GUICommon
import CommonTables

AbilityWindow = 0
TextAreaControl = 0
DoneButton = 0
AbilityTable = 0
Abclasrq = 0
Abclsmod = 0
Abracerq = 0
Abracead = 0
PointsLeft = 0
Minimum = 0
Maximum = 0
Add = 0
KitIndex = 0
HasStrExtra = 0
MyChar = 0
CurrentStat = 0

def CalcLimits(Abidx):
	global Minimum, Maximum, Add

	Race = CommonTables.Races.FindValue (3, GemRB.GetPlayerStat (MyChar, IE_RACE) )
	RaceName = CommonTables.Races.GetRowName(Race)

	Minimum = 3
	Maximum = 18

	tmp = Abclasrq.GetValue(KitIndex, Abidx)
	if tmp!=0 and tmp>Minimum:
		Minimum = tmp

	Race = Abracerq.GetRowIndex(RaceName)
	tmp = Abracerq.GetValue(Race, Abidx*2)
	if tmp!=0 and tmp>Minimum:
		Minimum = tmp

	tmp = Abracerq.GetValue(Race, Abidx*2+1)
	if tmp!=0 and tmp>Maximum:
		Maximum = tmp

	Race = Abracead.GetRowIndex(RaceName)
	Add = Abracead.GetValue(Race, Abidx)
	Maximum = Maximum + Add
	Minimum = Minimum + Add
	if Minimum<1:
		Minimum=1
	if Maximum>25:
		Maximum=25

	return

def UpdateLabels():
	global HasStrExtra
	for i in range(6):
		v = GemRB.GetVar("Ability " + str(i))
		Label = AbilityWindow.GetControl(0x10000003 + i)
		e = GemRB.GetVar("StrExtra")
		if i == 0 and v == 18 and HasStrExtra:
			Label.SetText("18/" + str(e))
		else:
			Label.SetText(str(v))
	SumLabel = AbilityWindow.GetControl(0x10000002)
	SumLabel.SetText(str(PointsLeft))

def SelectStat(idx):
	global CurrentStat
	CurrentStat = idx
	GemRB.SetVar("Ability", idx)
	CalcLimits(idx)
	GemRB.SetToken("MINIMUM", str(Minimum))
	GemRB.SetToken("MAXIMUM", str(Maximum))
	try:
		TextAreaControl.SetText(AbilityTable.GetValue(idx, 1))
	except Exception:
		pass

def DecreaseStat(idx):
	global PointsLeft, Minimum, Maximum, Add, HasStrExtra

	SelectStat(idx)
	Ability = GemRB.GetVar("Ability " + str(idx))
	if Ability <= Minimum:
		return

	GemRB.SetVar("Ability " + str(idx), Ability - 1)
	PointsLeft = PointsLeft + 1
	GemRB.SetVar("Ability -1", PointsLeft)
	UpdateLabels()

def IncreaseStat(idx):
	global PointsLeft, Minimum, Maximum, Add, HasStrExtra

	SelectStat(idx)
	if PointsLeft <= 0:
		return
	Ability = GemRB.GetVar("Ability " + str(idx))
	if Ability >= Maximum:
		return

	GemRB.SetVar("Ability " + str(idx), Ability + 1)
	PointsLeft = PointsLeft - 1
	GemRB.SetVar("Ability -1", PointsLeft)
	UpdateLabels()

def OnDpadUp():
	global CurrentStat
	CurrentStat = (CurrentStat - 1) % 6
	SelectStat(CurrentStat)

def OnDpadDown():
	global CurrentStat
	CurrentStat = (CurrentStat + 1) % 6
	SelectStat(CurrentStat)

def OnDpadLeft():
	global CurrentStat
	DecreaseStat(CurrentStat)

def OnDpadRight():
	global CurrentStat
	IncreaseStat(CurrentStat)

def RollPress():
	global Minimum, Maximum, Add, HasStrExtra, PointsLeft

	PointsLeft = 3 # 3 free points to customize stats immediately!
	GemRB.SetVar("Ability -1", PointsLeft)

	if HasStrExtra:
		e = GemRB.Roll(1, 100, 0)
	else:
		e = 0
	GemRB.SetVar("StrExtra", e)

	# Roll stats ensuring a good, playable hero (total >= 75)
	attempts = 0
	while attempts < 50:
		attempts += 1
		total = 0
		for i in range(6):
			CalcLimits(i)
			v = GemRB.Roll(3, 5, Add + 3)
			if v < Minimum: v = Minimum
			if v > Maximum: v = Maximum
			GemRB.SetVar("Ability " + str(i), v)
			total += v
		if total >= 75:
			break

	UpdateLabels()
	SelectStat(0)
	return

def StorePress():
	GemRB.SetVar("StoredStrExtra", GemRB.GetVar("StrExtra"))
	for i in range(-1, 6):
		GemRB.SetVar("Stored " + str(i), GemRB.GetVar("Ability " + str(i)))
	return

def RecallPress():
	global PointsLeft

	e = GemRB.GetVar("StoredStrExtra")
	GemRB.SetVar("StrExtra", e)
	for i in range(-1, 6):
		v = GemRB.GetVar("Stored " + str(i))
		GemRB.SetVar("Ability " + str(i), v)

	PointsLeft = GemRB.GetVar("Stored -1")
	GemRB.SetVar("Ability -1", PointsLeft)
	UpdateLabels()
	return

def NextPress():
	AbilityWindow.Close()
	AbilityTable = GemRB.LoadTable("ability")
	AbilityCount = AbilityTable.GetRowCount()

	for i in range(AbilityCount):
		StatID = AbilityTable.GetValue(i, 3)
		StatValue = GemRB.GetVar("Ability " + str(i))
		GemRB.SetPlayerStat(MyChar, StatID, StatValue)

	GemRB.SetPlayerStat(MyChar, IE_STREXTRA, GemRB.GetVar("StrExtra"))
	CharGenCommon.next()

def OnLoad():
	global AbilityWindow, TextAreaControl, DoneButton
	global PointsLeft, HasStrExtra
	global AbilityTable, Abclasrq, Abclsmod, Abracerq, Abracead
	global KitIndex, Minimum, Maximum, MyChar, CurrentStat

	Abracead = GemRB.LoadTable("ABRACEAD")
	Abclasrq = GemRB.LoadTable("ABCLASRQ")
	Abracerq = GemRB.LoadTable("ABRACERQ")

	MyChar = GemRB.GetVar("Slot")
	Kit = GUICommon.GetKitIndex(MyChar)
	ClassName = GUICommon.GetClassRowName(MyChar)
	if Kit == 0:
		KitName = ClassName
	else:
		KitName = CommonTables.KitList.GetValue(Kit, 0)

	HasStrExtra = CommonTables.Classes.GetValue(ClassName, "STREXTRA", GTV_INT)
	KitIndex = Abclasrq.GetRowIndex(KitName)

	AbilityTable = GemRB.LoadTable("ability")
	AbilityWindow = GemRB.LoadWindow(4, "GUICG")

	RerollButton = AbilityWindow.GetControl(2)
	RerollButton.SetText(11982)
	RerollButton.SetHotKey(' ', 0, True) # Button Y (Space) rerolls!
	RerollButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, RollPress)

	StoreButton = AbilityWindow.GetControl(37)
	StoreButton.SetText(17373)
	StoreButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, StorePress)

	RecallButton = AbilityWindow.GetControl(38)
	RecallButton.SetText(17374)
	RecallButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, RecallPress)

	BackButton = AbilityWindow.GetControl(36)
	BackButton.SetText(15416)
	BackButton.MakeEscape()
	BackButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, lambda: CharGenCommon.back(AbilityWindow))

	DoneButton = AbilityWindow.GetControl(0)
	DoneButton.SetText(11973)
	DoneButton.MakeDefault()
	DoneButton.SetState(IE_GUI_BUTTON_ENABLED)
	DoneButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, NextPress)

	TextAreaControl = AbilityWindow.GetControl(29)
	TextAreaControl.SetText(17247)

	CurrentStat = 0
	RollPress()
	StorePress()

	# Set up each ability row: label, minus (<), plus (>)
	for i in range(6):
		# Ability label / name
		BtnLabel = AbilityWindow.GetControl(i + 30)
		BtnLabel.SetEvent(IE_GUI_BUTTON_ON_PRESS, (lambda idx=i: lambda: SelectStat(idx))())

		# Minus button (<): DECREASE stat, increase pool
		BtnMinus = AbilityWindow.GetControl(i * 2 + 16)
		BtnMinus.SetEvent(IE_GUI_BUTTON_ON_PRESS, (lambda idx=i: lambda: DecreaseStat(idx))())
		BtnMinus.SetActionInterval(150)

		# Plus button (>): INCREASE stat, decrease pool
		BtnPlus = AbilityWindow.GetControl(i * 2 + 17)
		BtnPlus.SetEvent(IE_GUI_BUTTON_ON_PRESS, (lambda idx=i: lambda: IncreaseStat(idx))())
		BtnPlus.SetActionInterval(150)

	# D-Pad navigation controls
	UpBtn = AbilityWindow.CreateControl(9901, IE_GUI_BUTTON, 0, 0, 0, 0)
	UpBtn.SetHotKey(chr(0x81), 0, True) # D-Pad UP
	UpBtn.SetEvent(IE_GUI_BUTTON_ON_PRESS, OnDpadUp)

	DownBtn = AbilityWindow.CreateControl(9902, IE_GUI_BUTTON, 0, 0, 0, 0)
	DownBtn.SetHotKey(chr(0x82), 0, True) # D-Pad DOWN
	DownBtn.SetEvent(IE_GUI_BUTTON_ON_PRESS, OnDpadDown)

	LeftBtn = AbilityWindow.CreateControl(9903, IE_GUI_BUTTON, 0, 0, 0, 0)
	LeftBtn.SetHotKey(chr(0x83), 0, True) # D-Pad LEFT (Decrease)
	LeftBtn.SetEvent(IE_GUI_BUTTON_ON_PRESS, OnDpadLeft)

	RightBtn = AbilityWindow.CreateControl(9904, IE_GUI_BUTTON, 0, 0, 0, 0)
	RightBtn.SetHotKey(chr(0x84), 0, True) # D-Pad RIGHT (Increase)
	RightBtn.SetEvent(IE_GUI_BUTTON_ON_PRESS, OnDpadRight)

	AbilityWindow.ShowModal(MODAL_SHADOW_NONE)
	return
