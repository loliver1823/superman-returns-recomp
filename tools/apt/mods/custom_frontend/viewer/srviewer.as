// iter-465 viewer lane: THE COSTUME VIEWER page (docs/MODS.md "Costume viewer"). Compiled by
// tools/apt/as2asm.py into ../srviewer.aptasm and appended to SCRCOSTUMES' class InitAction
// (55_viewer.patch), next to _global.SRCostumes. It re-lays out the game's own COSTUMES screen in
// the Spider-Man (2000) COSTUME VIEWER arrangement from the game's own UI pieces (the ThumbViewer
// library's AdvancedText, Scrim, ThumbBorder, ThumbHighlight and ButtonHints): title top left, the
// suit list in a framed panel on the left, COSTUME / CHARACTER / COMMENTS on the right, the
// controls legend bottom left, the Fortress crystal art behind. The centre stays open: the host
// overlay draws the selected suit's 3D model there (kernel/costume_viewer.hpp, SR_CostumeViewer).
// Selection, WEAR and BACK stay this screen's (WEAR = the same SRCostumes / RecordAdapter calls as
// costumes_input.aptasm). ScrCostumes.ExitScreen calls Close and deletes _global.SRViewer.
var v = new Object();
_global.SRViewer = v;
v.rowsMax = 11;
v.rowStep = 36;
// ONE GRID (stage 1280x720; the Apt stage scales uniformly, so 4K = the same layout x3). Outer
// margin 40 on both sides; both panels the same size; 27 of side padding (the highlight bar's end
// caps sit 4 px inside the border and the row text clears them) and 20 of top / bottom padding
// inside each panel; 20 between the title, the panels and the hint row. Text fields draw their glyphs ~2-3 px right of / below
// their _x / _y (the PartText field bounds start at -2), hence the -2 / -3 below.
v.gridL = 40;
v.panelW = 380;
v.panelR = 860;     // right panel x = 1280 - 40 - panelW
v.panelT = 112;
v.panelH = 409;     // 20 + 11 rows x 36 + 20, bar insets equal at the top and the bottom
v.pad = 20;
v.gap = 20;
v.textX = 25;       // field _x inside a panel: glyphs at panel + 27
v.headGap = 24;     // heading _y -> its value's first line _y
v.nextHead = 34;    // a value's last line _y -> the next heading _y (22 px between the glyphs)

v.Open = function (screen) {
  var tv = screen.mThumbViewer;
  if (tv == undefined) {
    return false;
  }
  var fm = _global.FlowMgr;
  this.on = false;
  this.chars = new Object();
  this.notes = new Object();
  fm.SR_ViewerOn = function (on) {
    _global.SRViewer.on = on == 1;
  };
  fm.SR_ViewerInfo = function (id, ch, note) {
    _global.SRViewer.chars[id] = ch;
    _global.SRViewer.notes[id] = note;
  };
  fscommand("SR_CostumeViewer", "open");
  delete fm.SR_ViewerOn;
  delete fm.SR_ViewerInfo;
  if (!this.on) {
    return false;
  }
  this.screen = screen;
  this.tv = tv;
  this.data = tv.mThumbData;
  // the retail page: thumbnails, their name / description, the scroll arrows and the centre well
  tv.mThumbsHolder._visible = false;
  tv.mThumbNameTxt._visible = false;
  tv.mThumbDescrTxt._visible = false;
  if (tv.mUpArrow != undefined) {
    tv.mUpArrow._visible = false;
    tv.mDownArrow._visible = false;
  }
  _global.SetFortressDisplayWell(false);
  _global.ClearHints();
  this.Panel(tv, "SRV_ListBg", 3500, this.gridL, this.panelT, this.panelW, this.panelH);
  this.Panel(tv, "SRV_InfoBg", 3510, this.panelR, this.panelT, this.panelW, this.panelH);
  // title: glyphs on the panels' left edge, its baseline `gap` above the panels (caps 26 high at 40)
  this.title = this.Text(tv, "SRV_Title", 3600, this.gridL - 3, this.panelT - this.gap - 26, 760, 40, false);
  this.title.SetText("COSTUME VIEWER");
  this.hl = tv.attachMovie("ThumbHighlight", "SRV_Hl", 3610);
  this.rows = new Array();
  var i = 0;
  while (i < this.rowsMax) {
    this.rows[i] = this.Text(tv, "SRV_Row" + i, 3620 + i, this.gridL + this.textX, this.RowY(i),
                             this.panelW - 2 * this.textX, 16, false);
    i = i + 1;
  }
  // right panel: heading, value, section gap - flowed in Refresh (Flow) so a two-line value pushes
  // the next section down; headings and values share one left edge
  var ix = this.panelR + this.textX;
  var iw = this.panelW - 2 * this.textX;
  this.h1 = this.Head(tv, "SRV_H1", 3700, ix, 0, "COSTUME:");
  this.t1 = this.Lines(tv, "SRV_T1_", 3710, ix, 20, 3);
  this.h2 = this.Head(tv, "SRV_H2", 3702, ix, 0, "CHARACTER:");
  this.t2 = this.Lines(tv, "SRV_T2_", 3720, ix, 20, 2);
  this.h3 = this.Head(tv, "SRV_H3", 3704, ix, 0, "COMMENTS:");
  this.t3 = this.Lines(tv, "SRV_T3_", 3730, ix, 16, 7);
  this.wrapW = iw;
  // the game's own hint row (ButtonHints), one horizontal row along the bottom under the list:
  // real button actions only (right to left: BACK, WEAR COSTUME). WEAR shows A, the menus' select
  // (Enter on the keyboard); X wears as well.
  var loc = _global.LocalizationAdapter;
  var hints = tv.attachMovie("ButtonHints", "SRV_Hints", 3800);
  hints.ShowHints(loc.GetLocalizedText("Generic_BACK"), "B", loc.GetLocalizedText("Costume_Wear_Hint"), "A");
  // right-anchored row (ButtonHints.AddHint lays hints out leftwards from _x): move it so the
  // leftmost label's glyphs sit on the panels' left edge; its keycaps start `gap` below the panels
  hints._x = 0;
  var hx = 0;
  var hi = 0;
  while (hints["Hint" + hi] != undefined) {
    if (hints["Hint" + hi]._x < hx) {
      hx = hints["Hint" + hi]._x;
    }
    hi = hi + 1;
  }
  hints._x = this.gridL - 3 - hx;
  hints._y = this.panelT + this.panelH + this.gap + 9;
  this.sel = tv.GetSelected();
  if (this.sel == undefined) {
    this.sel = 0;
  }
  this.top = 0;
  this.popup = false;
  this.Refresh();
  this.timer = setInterval(this, "Tick", 100);
  return true;
};

v.Panel = function (tv, name, depth, x, y, w, h) {
  var s = tv.attachMovie("Scrim", name, depth);
  s._x = x;
  s._y = y;
  s._width = w;
  s._height = h;
  s._alpha = 62;
  var b = tv.attachMovie("ThumbBorder", name + "B", depth + 1);
  b._x = x;
  b._y = y;
  b.TopBorder._width = w;
  b.BottomBorder._width = w;
  var side = h - (b.TopBorder._height + b.BottomBorder._height);
  b.LeftBorder._height = side;
  b.RightBorder._height = side;
  b.TopBorder._x = 0;
  b.TopBorder._y = 0;
  b.LeftBorder._x = 0;
  b.BottomBorder._x = 0;
  b.LeftBorder._y = b.TopBorder._height;
  b.RightBorder._y = b.TopBorder._height;
  b.BottomBorder._y = h - b.BottomBorder._height;
  b.RightBorder._x = w - b.RightBorder._width;
};

v.Text = function (tv, name, depth, x, y, w, size, multi) {
  var t = tv.attachMovie("AdvancedText", name, depth);
  t.SetWidth(w);
  t.SetAutoSize("none");
  t.SetAlignment("left");
  t.SetFontSize(size);
  if (multi) {
    t.SetMultiline(true);
    t.SetWordWrap(true);
    t.SetAutoSize("left");
  } else {
    t.SetMultiline(false);
    t.SetWordWrap(false);
    t.SetAutoSize("left");
  }
  t._x = x;
  t._y = y;
  return t;
};

v.RowY = function (i) {
  return this.panelT + this.pad - 2 + i * this.rowStep;
};

// A wrapped value is drawn as single-line fields, one per line: Apt ignores TextFormat.leading and
// a multiline field's _height does not follow its text when read right after SetText (roll lb720),
// while a single line's measured width does (AdvancedText.mTextWidth = PartText.GetWidth after
// SetText; ButtonHints relies on the same). So: greedy word wrap measured on
// the line fields themselves, our own line pitch (size + 2), and a known height for the flow.
v.Lines = function (tv, name, depth, x, size, count) {
  var a = new Array();
  var i = 0;
  while (i < count) {
    a[i] = this.Text(tv, name + i, depth + i, x, 0, 760, size, false);
    i = i + 1;
  }
  a.pitch = size + 2;
  a.n = 1;
  return a;
};

v.Wrap = function (lines, text) {
  var s = "" + text;
  var words = s.split(" ");
  var k = 0;
  var cur = "";
  var i = 0;
  while (i < words.length) {
    var w = words[i];
    if (w != "") {
      var cand = w;
      if (cur != "") {
        cand = cur + " " + w;
      }
      lines[k].SetText(cand);
      if (cur != "" && lines[k].mTextWidth > this.wrapW && k < lines.length - 1) {
        lines[k].SetText(cur);
        k = k + 1;
        cur = w;
        lines[k].SetText(cur);
      } else {
        cur = cand;
      }
    }
    i = i + 1;
  }
  if (cur == "") {
    lines[0].SetText("");
  }
  var j = 0;
  while (j < lines.length) {
    lines[j]._visible = j <= k;
    j = j + 1;
  }
  lines.n = k + 1;
};

// the right panel's sections, top down from the list's first row line
v.Reflow = function () {
  var y = this.RowY(0);
  y = this.Flow(this.h1, this.t1, y);
  y = this.Flow(this.h2, this.t2, y);
  this.Flow(this.h3, this.t3, y);
};

// heading at y, its value's lines below, returns the next section's y
v.Flow = function (h, lines, y) {
  h._y = y;
  var vy = y + this.headGap;
  var i = 0;
  while (i < lines.length) {
    lines[i]._y = vy + i * lines.pitch;
    i = i + 1;
  }
  return vy + (lines.n - 1) * lines.pitch + this.nextHead;
};

v.Head = function (tv, name, depth, x, y, text) {
  var t = this.Text(tv, name, depth, x, y, 344, 18, false);
  t.SetFontColor(16765440);
  t.SetText(text);
  return t;
};

v.IdOf = function (k) {
  var r = this.data[k];
  if (r.SRId != undefined) {
    return r.SRId;
  }
  return "retail/" + (k + 1);
};

v.Show = function () {
  var lk = 0;
  if (this.data[this.sel].Locked) {
    lk = 1;
  }
  fscommand("SR_CostumeViewer", "show " + this.IdOf(this.sel) + " " + lk);
};

v.Refresh = function () {
  var n = this.data.length;
  if (this.sel < this.top) {
    this.top = this.sel;
  }
  if (this.sel >= this.top + this.rowsMax) {
    this.top = this.sel - this.rowsMax + 1;
  }
  var i = 0;
  while (i < this.rowsMax) {
    var k = this.top + i;
    var row = this.rows[i];
    if (k < n) {
      row._visible = true;
      if (this.data[k].Locked) {
        row.SetText("???");
      } else {
        row.SetText(this.data[k].Name);
      }
      if (k == this.sel) {
        row._alpha = 100;
      } else {
        row._alpha = 50;
      }
    } else {
      row._visible = false;
    }
    i = i + 1;
  }
  // the bar inside the list panel, 4 px from each 2 px border (its art starts 15 px left of _x at
  // this width), centred on the row's glyphs
  this.hl._width = this.panelW - 12;
  this.hl._height = 32;
  this.hl._x = this.gridL + 6 + 15;
  this.hl._y = this.RowY(this.sel - this.top) - 8;
  var r = this.data[this.sel];
  var id = this.IdOf(this.sel);
  if (r.Locked) {
    this.Wrap(this.t1, "???");
    this.Wrap(this.t2, "???");
    this.Wrap(this.t3, "???");
  } else {
    this.Wrap(this.t1, r.Name);
    var ch = this.chars[id];
    if (ch == undefined || ch == "") {
      ch = "SUPERMAN";
    }
    this.Wrap(this.t2, ch);
    var note = this.notes[id];
    if (note == undefined || note == "") {
      note = r.Description;
    }
    this.Wrap(this.t3, note);
  }
  this.Reflow();
  this.tv.mActiveThumb = this.sel;
  this.Show();
};

v.Tick = function () {
  var up = _global.gPopupMgr.aPopupStack.length > 0;
  if (up != this.popup) {
    this.popup = up;
    if (up) {
      fscommand("SR_CostumeViewer", "hide");
    } else {
      this.Show();
    }
  }
};

v.Wear = function () {
  var r = this.data[this.sel];
  if (r.Locked) {
    return false;
  }
  var c = _global.SRCostumes;
  if (r.SRId != undefined) {
    c.Wear(r.SRId, this.tv);
    if (!c.queued) {
      this.Wrap(this.t3, c.reply);
      this.Reflow();
    }
    return true;
  }
  // Retail false: queued 0 = the next-start note (shown), queued 2 = the host switched (no popup)
  if (c == undefined || c.Retail(this.sel, this.tv)) {
    _global.RecordAdapter.SetActiveCostume(this.sel);
  } else {
    if (c.queued != 2) {
      this.Wrap(this.t3, c.reply);
      this.Reflow();
    }
  }
  return true;
};

// true = handled here (HandleInput returns false at once); false = the retail page's input
v.Input = function (screen, key) {
  if (this.tv == undefined || this.screen != screen) {
    return false;
  }
  var K = _global.INPUTCODE;
  var n = this.data.length;
  if (key == K.UP || key == K.LAS_UP) {
    this.sel = this.sel - 1;
    if (this.sel < 0) {
      this.sel = n - 1;
    }
    _global.playSound(_global.SOUND.FOCUS);
    this.Refresh();
    return true;
  }
  if (key == K.DOWN || key == K.LAS_DOWN) {
    this.sel = this.sel + 1;
    if (this.sel >= n) {
      this.sel = 0;
    }
    _global.playSound(_global.SOUND.FOCUS);
    this.Refresh();
    return true;
  }
  // the right stick and the mouse turn / zoom the suit on the host; left / right and the stick's
  // own codes must not move the hidden thumbnail grid
  if (key == K.LEFT || key == K.RIGHT || key == K.RAS_LEFT || key == K.RAS_RIGHT || key == K.RAS_UP ||
      key == K.RAS_DOWN) {
    return true;
  }
  // A (Enter on the keyboard, the menus' select) and X both wear
  if (key == K.CROSS || key == K.SQUARE) {
    this.Wear();
    return true;
  }
  fscommand("SR_CostumeViewer", "key " + key);
  return false;
};

v.Close = function (screen) {
  if (this.timer != undefined) {
    clearInterval(this.timer);
  }
  if (this.tv != undefined) {
    fscommand("SR_CostumeViewer", "close");
  }
  this.tv = undefined;
  this.screen = undefined;
};
