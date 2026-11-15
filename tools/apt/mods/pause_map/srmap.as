// iter-466 map lane: THE USEFUL PAUSE MAP (docs/MAP.md). Compiled by tools/apt/as2asm.py into
// ../srmap.aptasm and appended to SCRMAP's ScrMap class InitAction (map.patch). A generic marker
// renderer: the host (kernel/map_markers.cpp, fscommand SR_Map) says what to mark - types (label,
// icon, colour, glyph, count) and markers (type, state, title, detail / status / record lines, a
// route) - and where, every map tick, through the game's own camera projection. This file knows
// nothing about races or kittens.
// Art: the screen's own pieces only - the objective reticle, its ring and pulse ring painted per
// colour by their own placement colour transforms (map.patch), the Superman icon, the screen's
// panel shape and its PartText font. Input (retail never used these on the map): the d-pad
// selects a marker (its route is drawn), LB / RB pick a legend row, X shows / hides that type.
// Observe-only: no game adapter that changes state is called; text keys go through
// LocalizationAdapter.GetLocalizedText (a lookup).
var m = new Object();
_global.SRMap = m;
m.on = false;
// the map window: the city between the top hint row (PAN / ZOOM) and the bottom hint row
m.left = 24;
m.top = 92;
m.right = 1256;
m.bottom = 628;
m.rowStep = 26;
m.legendW = 340;
m.lineW = 5;
m.edgeW = 9;
m.chevStep = 110;

m.Open = function (screen) {
  var fm = _global.FlowMgr;
  this.screen = screen;
  this.on = false;
  this.types = new Array();
  this.mk = new Array();
  this.sel = -1;
  this.row = 0;
  this.ticks = 0;
  this.route = new Array();
  this.routeN = 0;
  this.legsMade = 0;
  this.chevsMade = 0;
  this.endsLog = 0;
  fm.SR_MapBegin = function (nt, nm, flags) {
    var s = _global.SRMap;
    s.nt = nt;
    s.nm = nm;
    s.on = flags == 1;
  };
  fm.SR_MapType = function (i, packed, label) {
    var s = _global.SRMap;
    var t = new Object();
    var k = i % 65536;
    t.hidden = i >= 65536;
    t.icon = Math.floor(packed / 256);
    t.color = packed % 256;
    t.label = label;
    t.glyph = "";
    t.count = "";
    s.types[k] = t;
  };
  fm.SR_MapGlyph = function (i, g) {
    _global.SRMap.types[i].glyph = g;
  };
  fm.SR_MapCount = function (i, done, total) {
    _global.SRMap.types[i].count = done + "/" + total;
  };
  fm.SR_MapMk = function (i, packed, title) {
    var s = _global.SRMap;
    var o = new Object();
    o.type = packed % 256;
    o.state = Math.floor(packed / 256);
    o.title = title;
    o.detail = "";
    o.status = "";
    o.rec = "";
    o.x = 0;
    o.y = 0;
    o.has = false;
    s.mk[i] = o;
  };
  fm.SR_MapInfo = function (i, detail, status) {
    var o = _global.SRMap.mk[i];
    o.detail = detail;
    o.status = status;
  };
  fm.SR_MapRec = function (i, rec) {
    _global.SRMap.mk[i].rec = rec;
  };
  fm.SR_MapEnd = function (n) {
  };
  // positions arrive as stage pixels + 32768 (0 = none): this Apt's numbers are float32, and every
  // value stays positive
  fm.SR_MapXY = function (i, x, y) {
    _global.SRMap.Place(i, x, y);
  };
  fm.SR_MapRoute = function (i, n) {
    var s = _global.SRMap;
    s.routeN = n;
    s.route = new Array();
    var k = 0;
    while (k < n) {
      var p = new Object();
      p.has = false;
      s.route[k] = p;
      k = k + 1;
    }
  };
  fm.SR_MapRtXY = function (k, x, y) {
    _global.SRMap.RoutePt(k, x, y);
  };
  // a mod's own map art (docs/MAP.md 4.1 view kind "art"): its movie and its own size
  fm.SR_MapView = function (kind, movie, size) {
    var s = _global.SRMap;
    s.artMovie = movie;
    s.artW = Math.floor(size / 65536);
    s.artH = size % 65536;
  };
  fm.SR_MapArt = function (corner, x, y) {
    var s = _global.SRMap;
    if (corner == 0) {
      s.artX0 = x;
      s.artY0 = y;
    } else {
      s.PlaceArt(s.artX0, s.artY0, x, y);
    }
  };
  fscommand("SR_Map", "open");
  if (!this.on) {
    this.Unhook();
    return false;
  }
  this.Build();
  return true;
};

m.Unhook = function () {
  var fm = _global.FlowMgr;
  delete fm.SR_MapBegin;
  delete fm.SR_MapType;
  delete fm.SR_MapGlyph;
  delete fm.SR_MapCount;
  delete fm.SR_MapMk;
  delete fm.SR_MapInfo;
  delete fm.SR_MapRec;
  delete fm.SR_MapEnd;
  delete fm.SR_MapXY;
  delete fm.SR_MapRoute;
  delete fm.SR_MapRtXY;
  delete fm.SR_MapView;
  delete fm.SR_MapArt;
};

// icon 0 ring, 1 objective reticle, 2 Superman, 3 small ring, 4 pulse ring; colour = palette index
// every icon but Superman's is the tinted ring; the reticle adds four tinted arrows (Arrows) and the
// pulse is the ring scaled every tick (Tick)
m.Template = function (icon, color) {
  var sc = this.screen;
  if (icon == 2) {
    return sc.SRMapT2;
  }
  return sc["SRMapR" + color];
};

// the objective reticle's layout (char 10): four arrows (char 9, 16 px) around a 32 px ring,
// offsets from the centre at 100 %
m.Arrows = function (base, depth, color, alpha) {
  var sc = this.screen;
  var a = new Array();
  var k = 0;
  while (k < 4) {
    sc["SRMapA" + color].duplicateMovieClip(base + k, depth + k);
    var c = sc[base + k];
    c._alpha = alpha;
    c._visible = true;
    a[k] = c;
    k = k + 1;
  }
  a[2]._rotation = 90;
  a[3]._rotation = -90;
  return a;
};

// per arrow: x, y offset at 100 %, x / y scale sign (right, left = mirrored, bottom, top = mirrored)
m.arL = new Array(12.5, -7.3, 1, 1, -13, -7.3, -1, 1, 7.6, 12.8, 1, 1, 7.6, -13.3, 1, -1);

m.PlaceArrows = function (a, cx, cy, sc, v) {
  var L = this.arL;
  var k = 0;
  while (k < 4) {
    var c = a[k];
    c._x = cx + L[k * 4] * sc / 100;
    c._y = cy + L[k * 4 + 1] * sc / 100;
    c._xscale = sc * L[k * 4 + 2];
    c._yscale = sc * L[k * 4 + 3];
    c._visible = v;
    k = k + 1;
  }
};

m.Scale = function (icon) {
  if (icon == 3) {
    return 45;
  }
  if (icon == 2) {
    return 50;
  }
  if (icon == 1) {
    return 70;
  }
  return 100;
};

m.Text = function (s) {
  if (s == undefined || s == "") {
    return "";
  }
  if (s.substr(0, 1) == "@") {
    return _global.LocalizationAdapter.GetLocalizedText(s.substr(1));
  }
  return s;
};

// one PartText in the screen's font
m.Label = function (name, depth, size, w, align, text) {
  var t = this.screen.attachMovie("PartText", name, depth);
  t.SetFontSize(size);
  t.SetWidth(w);
  t.SetAlignment(align);
  t.SetAutoSize(align);
  t.SetText(text);
  return t;
};

// a text colour as 0..255 multipliers of the white font (as ion.utils.ColorUtil.HexToTransform)
m.Ink = function (clip, r, g, b) {
  var c = new Color(clip);
  var o = new Object();
  o.ra = r;
  o.ga = g;
  o.ba = b;
  c.setTransform(o);
};

m.HideTemplates = function () {
  var sc = this.screen;
  sc.SRMapT0._visible = false;
  sc.SRMapT1._visible = false;
  sc.SRMapT2._visible = false;
  sc.SRMapT3._visible = false;
  sc.SRMapLk._visible = false;
  var k = 0;
  while (k < 10) {
    sc["SRMapR" + k]._visible = false;
    sc["SRMapA" + k]._visible = false;
    sc["SRMapL" + k]._visible = false;
    k = k + 1;
  }
};

m.Build = function () {
  var sc = this.screen;
  this.HideTemplates();
  if (this.artMovie != undefined) {
    // under the screen's bars and text (depths 15..27), over its mask quad
    this.art = sc.createEmptyMovieClip("SRMapArt", 4);
    this.art.loadMovie(this.artMovie);
    this.art._visible = false;
    if (this.artX1 != undefined) {
      this.PlaceArt(this.artX0, this.artY0, this.artX1, this.artY1);
    }
  }
  // the game's own Superman icon and reticle stay on top of the markers
  sc.createEmptyMovieClip("SRMapTopA", 3990);
  sc.mSupermanIcon.swapDepths(sc.SRMapTopA);
  this.nRet = 0;
  this.hasPulse = false;
  this.pulseK = 1;
  var i = 0;
  while (i < this.nm) {
    var o = this.mk[i];
    var t = this.types[o.type];
    this.Template(t.icon, t.color).duplicateMovieClip("SRMapMk" + i, 1000 + i);
    var c = sc["SRMapMk" + i];
    c._xscale = this.Scale(t.icon);
    c._yscale = c._xscale;
    // completed: the dimmed variant
    if (o.state % 2 == 1) {
      c._alpha = 40;
    } else {
      c._alpha = 100;
    }
    o.clip = c;
    if (t.icon == 1) {
      o.arrows = this.Arrows("SRMapAr" + this.nRet + "_", 1800 + this.nRet * 4, t.color, c._alpha);
      this.nRet = this.nRet + 1;
    }
    if (t.icon == 4) {
      this.hasPulse = true;
    }
    if (t.glyph != "") {
      o.glyph = this.Label("SRMapGl" + i, 1500 + i, 12, 30, "center", t.glyph);
      o.glyph._alpha = c._alpha;
    }
    // positions that arrived with the open (before the clips existed)
    this.Show(o);
    i = i + 1;
  }
  this.BuildLegend();
  this.BuildBox();
  this.MakeEnds();
};

// the ring template (char 5) is anchored at its top-left corner (its 32 px shape at 0,0); the Superman
// icon is centred: the offset that puts a clip's centre on (x, y)
m.Off = function (icon) {
  if (icon == 2) {
    return 0;
  }
  return 16 * this.Scale(icon) / 100;
};

m.Show = function (o) {
  var v = this.Visible(o);
  var icon = this.types[o.type].icon;
  var off = this.Off(icon);
  if (icon == 4) {
    var ps = this.Scale(4) * this.pulseK;
    o.clip._xscale = ps;
    o.clip._yscale = ps;
    off = 16 * ps / 100;
  }
  o.clip._x = o.x - off;
  o.clip._y = o.y - off;
  o.clip._visible = v;
  if (o.arrows != undefined) {
    this.PlaceArrows(o.arrows, o.x, o.y, this.Scale(1), v);
  }
  if (o.glyph != undefined) {
    o.glyph._x = o.x - 15;
    o.glyph._y = o.y - 8;
    o.glyph._visible = v;
  }
};

m.Visible = function (o) {
  if (!o.has) {
    return false;
  }
  if (this.types[o.type].hidden) {
    return false;
  }
  if (o.x < this.left || o.x > this.right || o.y < this.top || o.y > this.bottom) {
    return false;
  }
  // never under the legend panel
  if (o.x > this.lx - 6 && o.x < this.lx + this.legendW + 6 && o.y > this.ly - 6 && o.y < this.ly + this.lh + 6) {
    return false;
  }
  return true;
};

m.Place = function (i, x, y) {
  var o = this.mk[i];
  if (o == undefined) {
    return;
  }
  if (x <= 0) {
    o.has = false;
  } else {
    o.has = true;
    o.x = x - 32768;
    o.y = y - 32768;
  }
  if (o.clip != undefined) {
    this.Show(o);
  }
};

// ---- the legend: title, then per type an icon, its label, its count and its ON / OFF state ----
m.BuildLegend = function () {
  var sc = this.screen;
  this.rows = new Array();
  this.lh = this.nt * this.rowStep + 82;
  this.lx = 40;
  this.ly = 100;
  sc.SRMapT3.duplicateMovieClip("SRMapLgBg", 3790);
  this.bg = sc.SRMapLgBg;
  this.bg._width = this.legendW;
  this.bg._height = this.lh;
  var bc = new Color(this.bg);
  var bo = new Object();
  bo.ra = 0;
  bo.ga = 0;
  bo.ba = 0;
  bo.aa = 235;
  bc.setTransform(bo);
  this.bg._visible = true;
  this.title = this.Label("SRMapLgTi", 3791, 18, 300, "left", "MAP KEY");
  this.Ink(this.title, 255, 200, 48);
  var i = 0;
  while (i < this.nt) {
    var t = this.types[i];
    var r = new Object();
    this.Template(t.icon, t.color).duplicateMovieClip("SRMapLgI" + i, 3800 + i);
    r.icon = sc["SRMapLgI" + i];
    r.icon._xscale = this.Scale(t.icon) * 0.7;
    r.icon._yscale = r.icon._xscale;
    r.icon._visible = true;
    if (t.icon == 1) {
      r.arrows = this.Arrows("SRMapLgA" + i + "_", 4000 + i * 4, t.color, 100);
    }
    if (t.glyph != "") {
      r.glyph = this.Label("SRMapLgG" + i, 3830 + i, 10, 30, "center", t.glyph);
    }
    r.label = this.Label("SRMapLgL" + i, 3850 + i, 15, 190, "left", t.label);
    r.count = this.Label("SRMapLgC" + i, 3870 + i, 15, 70, "right", t.count);
    r.onoff = this.Label("SRMapLgO" + i, 3890 + i, 13, 44, "right", "ON");
    this.rows[i] = r;
    i = i + 1;
  }
  this.hint = this.Label("SRMapLgH", 3910, 12, 320, "left", "UP / DOWN  TYPE      LEFT / RIGHT  NEXT");
  this.Ink(this.hint, 190, 190, 190);
  this.hint2 = this.Label("SRMapLgH2", 3911, 12, 320, "left", "A  NEXT      X  SHOW / HIDE");
  this.Ink(this.hint2, 190, 190, 190);
  this.MoveLegend(this.lx, this.ly);
  this.RefreshLegend();
};

m.MoveLegend = function (x, y) {
  this.lx = x;
  this.ly = y;
  this.bg._x = x;
  this.bg._y = y;
  this.title._x = x + 14;
  this.title._y = y + 8;
  var i = 0;
  while (i < this.nt) {
    var r = this.rows[i];
    var ry = y + 40 + i * this.rowStep;
    var lo = this.Off(this.types[i].icon) * 0.7;
    r.icon._x = x + 26 - lo;
    r.icon._y = ry + 10 - lo;
    if (r.arrows != undefined) {
      this.PlaceArrows(r.arrows, x + 26, ry + 10, this.Scale(1) * 0.7, true);
    }
    if (r.glyph != undefined) {
      r.glyph._x = x + 11;
      r.glyph._y = ry + 3;
    }
    r.label._x = x + 46;
    r.label._y = ry;
    r.count._x = x + 216;
    r.count._y = ry;
    r.onoff._x = x + this.legendW - 58;
    r.onoff._y = ry + 2;
    i = i + 1;
  }
  this.hint._x = x + 14;
  this.hint._y = y + this.lh - 38;
  this.hint2._x = x + 14;
  this.hint2._y = y + this.lh - 22;
};

m.RefreshLegend = function () {
  var i = 0;
  while (i < this.nt) {
    var r = this.rows[i];
    var t = this.types[i];
    var a = 100;
    if (t.hidden) {
      a = 40;
      r.onoff.SetText("OFF");
      this.Ink(r.onoff, 150, 150, 150);
    } else {
      r.onoff.SetText("ON");
      this.Ink(r.onoff, 120, 255, 140);
    }
    r.icon._alpha = a;
    r.label._alpha = a;
    r.count._alpha = a;
    if (r.glyph != undefined) {
      r.glyph._alpha = a;
    }
    if (i == this.row) {
      this.Ink(r.label, 255, 200, 48);
      this.Ink(r.count, 255, 200, 48);
    } else {
      this.Ink(r.label, 255, 255, 255);
      this.Ink(r.count, 255, 255, 255);
    }
    i = i + 1;
  }
};

// once the map camera settled: the legend goes to the corner that covers the fewest markers
m.PlaceLegend = function () {
  var xs = new Array();
  var ys = new Array();
  xs[0] = 40;
  ys[0] = 100;
  xs[1] = 1280 - 40 - this.legendW;
  ys[1] = 100;
  xs[2] = 40;
  ys[2] = 624 - this.lh;
  xs[3] = 1280 - 40 - this.legendW;
  ys[3] = 624 - this.lh;
  var best = 0;
  var bestN = 100000;
  var k = 0;
  while (k < 4) {
    var n = 0;
    var i = 0;
    while (i < this.nm) {
      var o = this.mk[i];
      if (o.has && !this.types[o.type].hidden && o.x > xs[k] && o.x < xs[k] + this.legendW && o.y > ys[k] && o.y < ys[k] + this.lh) {
        n = n + 1;
      }
      i = i + 1;
    }
    if (n < bestN) {
      bestN = n;
      best = k;
    }
    k = k + 1;
  }
  this.MoveLegend(xs[best], ys[best]);
  this.ApplyFilters();
};

// ---- the selection box: name, detail, status, best / target ----
m.BuildBox = function () {
  var sc = this.screen;
  sc.SRMapT3.duplicateMovieClip("SRMapBxBg", 3950);
  this.box = sc.SRMapBxBg;
  var bc = new Color(this.box);
  var bo = new Object();
  bo.ra = 0;
  bo.ga = 0;
  bo.ba = 0;
  bo.aa = 235;
  bc.setTransform(bo);
  this.bt = this.Label("SRMapBxT", 3951, 18, 520, "center", " ");
  this.Ink(this.bt, 255, 200, 48);
  this.b1 = this.Label("SRMapBx1", 3952, 13, 520, "center", " ");
  this.b2 = this.Label("SRMapBx2", 3953, 14, 520, "center", " ");
  this.b3 = this.Label("SRMapBx3", 3954, 14, 520, "center", " ");
  this.Ink(this.b3, 140, 220, 255);
  this.ShowBox(false);
};

m.ShowBox = function (on) {
  this.box._visible = on;
  this.bt._visible = on;
  this.b1._visible = on;
  this.b2._visible = on;
  this.b3._visible = on;
};

// the detail box sits at the bottom centre (the game's own objective-text place) unless the selected
// marker or its route's START would be under it; then it moves to the top of the map window
m.Covered = function (x, y) {
  return x > 360 && x < 920 && y > 590 - this.boxH - 24 && y < 610;
};

m.PlaceBox = function () {
  if (this.boxH == undefined) {
    return;
  }
  var h = this.boxH;
  var x = 380;
  var y = 590 - h;
  var top = false;
  if (this.sel >= 0) {
    var o = this.mk[this.sel];
    if (o.has && this.Covered(o.x, o.y)) {
      top = true;
    }
  }
  if (this.routeN > 1) {
    var s = this.route[0];
    if (s.has && this.Covered(s.x, s.y)) {
      top = true;
    }
  }
  if (top) {
    y = 100;
  }
  this.box._x = x;
  this.box._y = y;
  this.box._width = 520;
  this.box._height = h;
  this.bt._x = x;
  this.bt._y = y + 6;
  this.b1._x = x;
  this.b1._y = y + 32;
  this.b2._x = x;
  this.b2._y = y + 53;
  this.b3._x = x;
  this.b3._y = y + 74;
};

m.FillBox = function (o) {
  var lines = new Array();
  var d = this.Text(o.detail);
  if (d != "") {
    lines[lines.length] = d;
  }
  if (o.status != "") {
    lines[lines.length] = this.Text(o.status);
  }
  if (o.rec != "") {
    lines[lines.length] = o.rec;
  }
  var title = this.Text(o.title);
  if (title == "") {
    title = this.types[o.type].label;
  }
  this.bt.SetText(title);
  var n = lines.length;
  if (n > 3) {
    n = 3;
  }
  this.boxH = 34 + n * 21;
  this.boxN = n;
  var k = 0;
  while (k < 3) {
    var t = this.b1;
    if (k == 1) {
      t = this.b2;
    }
    if (k == 2) {
      t = this.b3;
    }
    if (k < n) {
      t.SetText(lines[k]);
      t._visible = true;
    } else {
      t._visible = false;
    }
    k = k + 1;
  }
  this.PlaceBox();
  this.box._visible = true;
  this.bt._visible = true;
};

// ---- the selected race's TRAIL: a line through every waypoint in order (a dark edge under the race
// colour), chevrons pointing the way, START and FINISH; each leg clipped to the map window. Depths:
// trail 100..999 under the markers (1000+), glyphs (1500+), reticle arrows (1800+), START/FINISH 3602+ ----
m.RoutePt = function (k, x, y) {
  var q = this.route[k];
  if (q == undefined) {
    return;
  }
  if (x <= 0) {
    q.has = false;
  } else {
    q.has = true;
    q.x = x - 32768;
    q.y = y - 32768;
  }
  this.routeDirty = true;
};

m.In = function (x, y) {
  return x > this.left && x < this.right && y > this.top && y < this.bottom;
};

// the part of a -> b inside the map window (Liang-Barsky): sets this.c0 / this.c1 (0..1) or false
m.Clip = function (ax, ay, dx, dy) {
  var t0 = 0;
  var t1 = 1;
  var p = new Array(0 - dx, dx, 0 - dy, dy);
  var q = new Array(ax - this.left, this.right - ax, ay - this.top, this.bottom - ay);
  var i = 0;
  while (i < 4) {
    if (p[i] == 0) {
      if (q[i] < 0) {
        return false;
      }
    } else {
      var r = q[i] / p[i];
      if (p[i] < 0) {
        if (r > t1) {
          return false;
        }
        if (r > t0) {
          t0 = r;
        }
      } else {
        if (r < t0) {
          return false;
        }
        if (r < t1) {
          t1 = r;
        }
      }
    }
    i = i + 1;
  }
  this.c0 = t0;
  this.c1 = t1;
  return true;
};

// one bar from (x, y), length len, angle deg, thickness w: the panel art (185 x 100, top-left anchored)
m.Bar = function (c, x, y, len, deg, w) {
  var rad = deg * 3.14159265 / 180;
  c._rotation = 0;
  c._xscale = len * 100 / 185;
  c._yscale = w;
  c._rotation = deg;
  c._x = x + Math.sin(rad) * w / 2;
  c._y = y - Math.cos(rad) * w / 2;
  c._visible = true;
};

m.Leg = function (k, color) {
  var sc = this.screen;
  while (this.legsMade <= k) {
    var n = this.legsMade;
    sc.SRMapLk.duplicateMovieClip("SRMapTe" + n, 100 + n);
    sc["SRMapL" + color].duplicateMovieClip("SRMapTl" + n, 360 + n);
    sc["SRMapTl" + n].color = color;
    this.legsMade = n + 1;
  }
  if (sc["SRMapTl" + k].color != color) {
    sc["SRMapL" + color].duplicateMovieClip("SRMapTl" + k, 360 + k);
    sc["SRMapTl" + k].color = color;
  }
};

m.Chev = function (j) {
  var sc = this.screen;
  while (this.chevsMade <= j) {
    sc.SRMapA0.duplicateMovieClip("SRMapTc" + this.chevsMade, 620 + this.chevsMade);
    this.chevsMade = this.chevsMade + 1;
  }
  return sc["SRMapTc" + j];
};

m.DrawRoute = function () {
  this.routeDirty = false;
  var sc = this.screen;
  var color = 0;
  if (this.sel >= 0) {
    color = this.types[this.mk[this.sel].type].color;
  }
  var legs = 0;
  var chev = 0;
  var k = 0;
  while (k + 1 < this.routeN && k < 255) {
    var a = this.route[k];
    var b = this.route[k + 1];
    var shown = false;
    if (a.has && b.has) {
      var dx = b.x - a.x;
      var dy = b.y - a.y;
      if (this.Clip(a.x, a.y, dx, dy)) {
        var x0 = a.x + dx * this.c0;
        var y0 = a.y + dy * this.c0;
        var full = Math.sqrt(dx * dx + dy * dy);
        var len = full * (this.c1 - this.c0);
        if (len > 0.5) {
          var deg = Math.atan2(dy, dx) * 57.2957795;
          this.Leg(k, color);
          this.Bar(sc["SRMapTe" + k], x0 - dx / full * 2, y0 - dy / full * 2, len + 4, deg, this.edgeW);
          this.Bar(sc["SRMapTl" + k], x0, y0, len, deg, this.lineW);
          shown = true;
          // chevrons: every chevStep px of the visible part, pointing from a to b
          var d = this.chevStep / 2;
          while (d < len && chev < 380) {
            var cv = this.Chev(chev);
            var rad = deg * 3.14159265 / 180;
            var cx = x0 + Math.cos(rad) * d;
            var cy = y0 + Math.sin(rad) * d;
            cv._rotation = 0;
            cv._xscale = 85;
            cv._yscale = 85;
            cv._rotation = deg;
            cv._x = cx - (Math.cos(rad) - Math.sin(rad)) * 6.8;
            cv._y = cy - (Math.sin(rad) + Math.cos(rad)) * 6.8;
            cv._visible = true;
            chev = chev + 1;
            d = d + this.chevStep;
          }
        }
      }
    }
    if (!shown && k < this.legsMade) {
      sc["SRMapTe" + k]._visible = false;
      sc["SRMapTl" + k]._visible = false;
    }
    k = k + 1;
  }
  while (k < this.legsMade) {
    sc["SRMapTe" + k]._visible = false;
    sc["SRMapTl" + k]._visible = false;
    k = k + 1;
  }
  while (chev < this.chevsMade) {
    sc["SRMapTc" + chev]._visible = false;
    chev = chev + 1;
  }
  this.Ends();
};

// START (green ring) and FINISH (gold ring + arrows), made with the screen (Build). Property names in
// this Apt are case-insensitive: "endS" was the method "Ends" (rolls race96_L / race96_M: START never
// showed), so no member here may differ from another only by case
m.MakeEnds = function () {
  var sc = this.screen;
  if (true) {
    this.Template(0, 5).duplicateMovieClip("SRMapRs", 3602);
    this.stClip = sc.SRMapRs;
    this.Template(0, 1).duplicateMovieClip("SRMapRf", 3603);
    this.fnClip = sc.SRMapRf;
    this.fnClip._xscale = 60;
    this.fnClip._yscale = 60;
    this.fnArrows = this.Arrows("SRMapRfA", 3606, 1, 100);
    this.stLabel = this.Label("SRMapRsL", 3604, 13, 80, "center", "START");
    this.Ink(this.stLabel, 64, 255, 128);
    this.fnLabel = this.Label("SRMapRfL", 3605, 13, 80, "center", "FINISH");
    this.Ink(this.fnLabel, 255, 200, 48);
  }
  this.stClip._visible = false;
  this.stLabel._visible = false;
  this.fnClip._visible = false;
  this.fnLabel._visible = false;
};

m.Ends = function () {
  if (this.stClip == undefined) {
    return;
  }
  var s = this.route[0];
  var f = this.route[this.routeN - 1];
  var on = this.routeN > 1;
  var sOn = on && s.has && this.In(s.x, s.y);
  var fOn = on && f.has && this.In(f.x, f.y);
  this.stClip._visible = sOn;
  this.stLabel._visible = sOn;
  this.fnClip._visible = fOn;
  this.fnLabel._visible = fOn;
  this.PlaceBox();
  if (sOn) {
    this.stClip._x = s.x - this.Off(0);
    this.stClip._y = s.y - this.Off(0);
    this.stLabel._x = s.x - 40;
    this.stLabel._y = s.y + 12;
  }
  if (this.fnArrows != undefined) {
    this.PlaceArrows(this.fnArrows, f.x, f.y, 60, fOn);
  }
  if (fOn) {
    this.fnClip._x = f.x - 16 * 0.6;
    this.fnClip._y = f.y - 16 * 0.6;
    this.fnLabel._x = f.x - 40;
    this.fnLabel._y = f.y + 16;
  }
  if (this.endsLog < 3) {
    this.endsLog = this.endsLog + 1;
    fscommand("SR_Map", "dbg ends " + sOn + " " + this.stClip._visible + " " + this.stClip._x + " " + this.stClip._y + " " + fOn);
  }
};

m.ApplyFilters = function () {
  var i = 0;
  while (i < this.nm) {
    var o = this.mk[i];
    if (o.clip != undefined) {
      this.Show(o);
    }
    i = i + 1;
  }
  if (this.sel >= 0 && !this.Visible(this.mk[this.sel])) {
    this.Deselect();
  }
};

m.Tick = function (screen, x, y) {
  if (!this.on || screen != this.screen) {
    return;
  }
  fscommand("SR_Map", "tick " + Math.floor(x * 10) / 10 + " " + Math.floor(y * 10) / 10);
  this.ticks = this.ticks + 1;
  if (this.ticks == 75) {
    this.PlaceLegend();
    // the current objective (an engaged trigger) is selected at once: its box and its route
    if (this.sel < 0) {
      var i = 0;
      while (i < this.nm) {
        if (Math.floor(this.mk[i].state / 2) % 2 == 1 && this.Visible(this.mk[i])) {
          this.Select(i);
          i = this.nm;
        }
        i = i + 1;
      }
    }
  }
  if (this.hasPulse) {
    this.pulseK = 1 + 0.125 * (1 + Math.sin(this.ticks * 0.21));
    var pi = 0;
    while (pi < this.nm) {
      var po = this.mk[pi];
      if (po.clip != undefined && this.types[po.type].icon == 4) {
        this.Show(po);
      }
      pi = pi + 1;
    }
  }
  if (this.sel >= 0) {
    this.MoveReticle();
  }
  if (this.routeDirty) {
    this.DrawRoute();
  }
};

m.MoveReticle = function () {
  var o = this.mk[this.sel];
  var r = this.screen.mcReticle;
  if (!this.Visible(o)) {
    r._visible = false;
    return;
  }
  r._x = o.x;
  r._y = o.y;
  r._visible = true;
};

m.Select = function (i) {
  this.sel = i;
  this.FillBox(this.mk[i]);
  this.MoveReticle();
  this.screen.mcReticle.Show();
  // the host answers with the marker's route (SR_MapRoute + SR_MapRtXY), drawn at once
  fscommand("SR_Map", "select " + i);
  this.DrawRoute();
};

m.Deselect = function () {
  this.sel = -1;
  this.screen.mcReticle._visible = false;
  this.ShowBox(false);
  fscommand("SR_Map", "select -1");
  this.DrawRoute();
};

// the next (dir 1) or previous (dir -1) marker of the legend's chosen type: one on screen first,
// any of that type otherwise
m.Next = function (dir) {
  var pick = -1;
  var any = -1;
  var n = 1;
  while (n <= this.nm) {
    var j = (this.sel + dir * n) % this.nm;
    if (this.sel < 0 && dir < 0) {
      j = (this.nm - n) % this.nm;
    }
    if (j < 0) {
      j = j + this.nm;
    }
    var q = this.mk[j];
    if (q.type == this.row && q.has && !this.types[q.type].hidden) {
      if (any < 0) {
        any = j;
      }
      if (this.Visible(q)) {
        pick = j;
        n = this.nm;
      }
    }
    n = n + 1;
  }
  if (pick < 0) {
    pick = any;
  }
  return pick;
};

// true = handled here (ScrMap.HandleInput returns at once); false = the screen's own input.
// The codes that reach ScrMap.HandleInput (rolls ch2_k: [MAP] key lines): the d-pad 37..40, A = CROSS
// 302, X = SQUARE 305, B / Y = the retail exit / Fortress. LB / RB never arrive (the pause flow keeps
// them), so the d-pad drives the legend: UP / DOWN choose the type, LEFT / RIGHT / A step through it.
m.Input = function (screen, key) {
  if (!this.on || screen != this.screen) {
    return false;
  }
  var K = _global.INPUTCODE;
  if (key == K.UP || key == K.DOWN) {
    if (key == K.UP) {
      this.row = this.row - 1;
    } else {
      this.row = this.row + 1;
    }
    if (this.row < 0) {
      this.row = this.nt - 1;
    }
    if (this.row >= this.nt) {
      this.row = 0;
    }
    _global.playSound(_global.SOUND.FOCUS);
    this.RefreshLegend();
    return true;
  }
  if (key == K.LEFT || key == K.RIGHT || key == K.CROSS) {
    var dir = 1;
    if (key == K.LEFT) {
      dir = -1;
    }
    var pick = this.Next(dir);
    if (pick >= 0) {
      _global.playSound(_global.SOUND.FOCUS);
      this.Select(pick);
    }
    return true;
  }
  if (key == K.SQUARE) {
    var t = this.types[this.row];
    t.hidden = !t.hidden;
    fscommand("SR_Map", "filter " + this.row + " " + (1 - t.hidden));
    _global.playSound(_global.SOUND.TOGGLE);
    this.RefreshLegend();
    this.ApplyFilters();
    return true;
  }
  return false;
};

m.PlaceArt = function (x0, y0, x1, y1) {
  this.artX0 = x0;
  this.artY0 = y0;
  this.artX1 = x1;
  this.artY1 = y1;
  var c = this.art;
  if (c == undefined) {
    return;
  }
  if (x0 <= 0 || x1 <= 0) {
    c._visible = false;
    return;
  }
  x0 = x0 - 32768;
  y0 = y0 - 32768;
  x1 = x1 - 32768;
  y1 = y1 - 32768;
  c._x = x0;
  c._y = y0;
  c._xscale = 100 * (x1 - x0) / this.artW;
  c._yscale = 100 * (y1 - y0) / this.artH;
  c._visible = true;
};

m.Close = function (screen) {
  if (!this.on) {
    return;
  }
  var sc = this.screen;
  var i = 0;
  while (i < this.nm) {
    var o = this.mk[i];
    if (o.clip != undefined) {
      o.clip.removeMovieClip();
    }
    if (o.glyph != undefined) {
      o.glyph.removeMovieClip();
    }
    if (o.arrows != undefined) {
      var ak = 0;
      while (ak < 4) {
        o.arrows[ak].removeMovieClip();
        ak = ak + 1;
      }
    }
    i = i + 1;
  }
  i = 0;
  while (i < this.nt) {
    var r = this.rows[i];
    r.icon.removeMovieClip();
    r.label.removeMovieClip();
    r.count.removeMovieClip();
    r.onoff.removeMovieClip();
    if (r.glyph != undefined) {
      r.glyph.removeMovieClip();
    }
    if (r.arrows != undefined) {
      var lk = 0;
      while (lk < 4) {
        r.arrows[lk].removeMovieClip();
        lk = lk + 1;
      }
    }
    i = i + 1;
  }
  i = 0;
  while (i < this.legsMade) {
    sc["SRMapTe" + i].removeMovieClip();
    sc["SRMapTl" + i].removeMovieClip();
    i = i + 1;
  }
  i = 0;
  while (i < this.chevsMade) {
    sc["SRMapTc" + i].removeMovieClip();
    i = i + 1;
  }
  if (this.stClip != undefined) {
    this.stClip.removeMovieClip();
    this.fnClip.removeMovieClip();
    this.stLabel.removeMovieClip();
    this.fnLabel.removeMovieClip();
    var ek = 0;
    while (ek < 4) {
      this.fnArrows[ek].removeMovieClip();
      ek = ek + 1;
    }
  }
  this.title.removeMovieClip();
  this.hint.removeMovieClip();
  this.hint2.removeMovieClip();
  this.bg.removeMovieClip();
  this.box.removeMovieClip();
  this.bt.removeMovieClip();
  this.b1.removeMovieClip();
  this.b2.removeMovieClip();
  this.b3.removeMovieClip();
  if (this.art != undefined) {
    this.art.removeMovieClip();
    this.art = undefined;
  }
  this.artMovie = undefined;
  // the next open builds its own START / FINISH (these clips went with this screen)
  this.stClip = undefined;
  this.fnArrows = undefined;
  this.Unhook();
  fscommand("SR_Map", "close");
  this.on = false;
  this.screen = undefined;
};
