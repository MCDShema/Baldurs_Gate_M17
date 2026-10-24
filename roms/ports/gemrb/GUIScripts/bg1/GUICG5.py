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
#character generation, name (GUICG5) with On-Screen Virtual Keyboard
import GemRB
from GUIDefines import *

import CharGenCommon
import GUICommon

NameWindow = 0
NameField = 0
DoneButton = 0
KeyButtons = []
KeyboardMode = "ENG"

ENG_ROWS = [
	["Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"],
	["A", "S", "D", "F", "G", "H", "J", "K", "L"],
	["Z", "X", "C", "V", "B", "N", "M"]
]

UKR_ROWS = [
	["Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З"],
	["Ф", "І", "В", "А", "П", "Р", "О", "Л", "Д"],
	["Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю"]
]

PRESETS = ["Abdel", "Gorion", "Bhaal", "Drizzt", "Elminster"]

def OnLetter(ch):
	cur = NameField.QueryText()
	if len(cur) < 14:
		c = ch.upper() if len(cur) == 0 else ch.lower()
		NameField.SetText(cur + c)
		EditChange()

def OnDel():
	cur = NameField.QueryText()
	if len(cur) > 0:
		NameField.SetText(cur[:-1])
		EditChange()

def OnClear():
	NameField.SetText("")
	EditChange()

def OnPreset(name):
	NameField.SetText(name)
	EditChange()

def OnToggleLang(btn):
	global KeyboardMode, KeyButtons
	KeyboardMode = "UKR" if KeyboardMode == "ENG" else "ENG"
	btn.SetText("ENG" if KeyboardMode == "ENG" else "УКР")
	rows = UKR_ROWS if KeyboardMode == "UKR" else ENG_ROWS
	idx = 0
	for row in rows:
		for ch in row:
			if idx < len(KeyButtons):
				KeyButtons[idx].SetText(ch)
				KeyButtons[idx].SetEvent(IE_GUI_BUTTON_ON_PRESS, (lambda c=ch: lambda: OnLetter(c))())
			idx += 1

def BuildKeyboard(window, start_y):
	global KeyButtons, KeyboardMode
	KeyButtons = []
	btn_id = 9100

	btn_w = 26
	btn_h = 24
	spacing = 3

	rows = UKR_ROWS if KeyboardMode == "UKR" else ENG_ROWS
	y = start_y
	for row in rows:
		row_w = len(row) * (btn_w + spacing) - spacing
		start_x = max(10, (360 - row_w) // 2)
		x = start_x
		for ch in row:
			btn = window.CreateControl(btn_id, IE_GUI_BUTTON, x, y, btn_w, btn_h)
			btn.SetText(ch)
			btn.SetEvent(IE_GUI_BUTTON_ON_PRESS, (lambda c=ch: lambda: OnLetter(c))())
			KeyButtons.append(btn)
			btn_id += 1
			x += btn_w + spacing
		y += btn_h + spacing

	# Function Row: Space, Del, Clear, Lang
	y += 2
	fx = 15
	sp_btn = window.CreateControl(btn_id, IE_GUI_BUTTON, fx, y, 75, btn_h)
	sp_btn.SetText("Пробіл")
	sp_btn.SetEvent(IE_GUI_BUTTON_ON_PRESS, lambda: OnLetter(" "))
	btn_id += 1
	fx += 80

	del_btn = window.CreateControl(btn_id, IE_GUI_BUTTON, fx, y, 75, btn_h)
	del_btn.SetText("<- Стерти")
	del_btn.SetEvent(IE_GUI_BUTTON_ON_PRESS, OnDel)
	btn_id += 1
	fx += 80

	clr_btn = window.CreateControl(btn_id, IE_GUI_BUTTON, fx, y, 75, btn_h)
	clr_btn.SetText("Очистити")
	clr_btn.SetEvent(IE_GUI_BUTTON_ON_PRESS, OnClear)
	btn_id += 1
	fx += 80

	lang_btn = window.CreateControl(btn_id, IE_GUI_BUTTON, fx, y, 70, btn_h)
	lang_btn.SetText("УКР" if KeyboardMode == "UKR" else "ENG")
	lang_btn.SetEvent(IE_GUI_BUTTON_ON_PRESS, lambda: OnToggleLang(lang_btn))
	btn_id += 1

	# Preset Names Row
	y += btn_h + spacing + 2
	px = 15
	pw = 58
	for pname in PRESETS:
		pbtn = window.CreateControl(btn_id, IE_GUI_BUTTON, px, y, pw, btn_h)
		pbtn.SetText(pname)
		pbtn.SetEvent(IE_GUI_BUTTON_ON_PRESS, (lambda n=pname: lambda: OnPreset(n))())
		btn_id += 1
		px += pw + spacing

def OnLoad():
	global NameWindow, NameField, DoneButton
	
	NameWindow = GemRB.LoadWindow(5, "GUICG")

	# Expand window height to host on-screen keyboard
	wframe = NameWindow.GetFrame()
	if wframe['h'] < 360:
		new_y = max(10, wframe['y'] - 50)
		NameWindow.SetPos(wframe['x'], new_y)
		NameWindow.SetSize(wframe['w'], 360)

	BackButton = NameWindow.GetControl(3)
	BackButton.SetText(15416)
	BackButton.MakeEscape()

	DoneButton = NameWindow.GetControl(0)
	DoneButton.SetText(11973)
	DoneButton.MakeDefault()
	DoneButton.SetState(IE_GUI_BUTTON_ENABLED)

	NameField = NameWindow.GetControl(2)
	cur_name = GemRB.GetToken("CHARNAME")
	if not cur_name or len(cur_name) == 0:
		cur_name = "Abdel"
		GemRB.SetToken("CHARNAME", cur_name)
	NameField.SetText(cur_name)
	EditChange()

	DoneButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, NextPress)
	BackButton.SetEvent(IE_GUI_BUTTON_ON_PRESS, lambda: CharGenCommon.back(NameWindow))
	NameField.SetEvent(IE_GUI_EDIT_ON_CHANGE, EditChange)

	# Build On-Screen Keyboard right under the NameField
	BuildKeyboard(NameWindow, 135)

	NameWindow.ShowModal(MODAL_SHADOW_NONE)
	NameField.Focus()
	return

def NextPress():
	NameWindow.Close()
	Name = NameField.QueryText()
	MyChar = GemRB.GetVar("Slot")
	GemRB.SetPlayerName(MyChar, Name, 0)
	GemRB.SetToken("CHARNAME", Name)
	CharGenCommon.next()
	return

def EditChange():
	Name = NameField.QueryText()
	if len(Name) == 0:
		DoneButton.SetState(IE_GUI_BUTTON_DISABLED)
	else:
		DoneButton.SetState(IE_GUI_BUTTON_ENABLED)
	return
