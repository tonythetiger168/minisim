// LEF/DEF reader and writer (src/lef.hpp, src/def.hpp): a small technology
// and a routed design are read, checked, written and read back.
#include "def.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>

static const char* LEF = R"(
VERSION 5.8 ;
BUSBITCHARS "[]" ;
DIVIDERCHAR "/" ;
UNITS
  DATABASE MICRONS 2000 ;
END UNITS
MANUFACTURINGGRID 0.005 ;
LAYER metal1
  TYPE ROUTING ;
  SPACING 0.065 ;
  WIDTH 0.07 ;
  PITCH 0.14 ;
  DIRECTION HORIZONTAL ;
  OFFSET 0.095 0.07 ;
  RESISTANCE RPERSQ 0.38 ;
  CAPACITANCE CPERSQDIST 7.7e-05 ;
  EDGECAPACITANCE 2.7e-05 ;
  THICKNESS 0.13 ;
END metal1
LAYER via1
  TYPE CUT ;
  SPACING 0.08 ;
  WIDTH 0.07 ;
  RESISTANCE 5 ;
END via1
LAYER metal2
  TYPE ROUTING ;
  SPACINGTABLE
    PARALLELRUNLENGTH 0.0 2.7
      WIDTH 0.0 0.07 0.07
      WIDTH 0.9 0.07 0.09 ;
  WIDTH 0.07 ;
  PITCH 0.19 0.14 ;
  DIRECTION VERTICAL ;
END metal2
VIA via1_0 DEFAULT
  LAYER metal1 ;
    RECT -0.035 -0.07 0.035 0.07 ;
  LAYER via1 ;
    RECT -0.035 -0.035 0.035 0.035 ;
  LAYER metal2 ;
    RECT -0.035 -0.07 0.035 0.07 ;
END via1_0
NONDEFAULTRULE wide
  LAYER metal2
    WIDTH 0.14 ;
    SPACING 0.14 ;
  END metal2
END wide
SITE core
  SYMMETRY Y ;
  CLASS CORE ;
  SIZE 0.19 BY 1.4 ;
END core
MACRO INV_X1
  CLASS CORE ;
  ORIGIN 0 0 ;
  SIZE 0.38 BY 1.4 ;
  SYMMETRY X Y ;
  SITE core ;
  PIN A
    DIRECTION INPUT ;
    USE SIGNAL ;
    PORT
      LAYER metal1 ;
        RECT 0.06 0.525 0.165 0.7 ;
    END
  END A
  PIN ZN
    DIRECTION OUTPUT ;
    PORT
      LAYER metal1 ;
        POLYGON 0.23 0.15 0.3 0.15 0.3 1.25 0.23 1.25 0.23 0.8 0.2 0.8 0.2 0.5 0.23 0.5 ;
    END
  END ZN
  PIN VDD
    DIRECTION INOUT ;
    USE POWER ;
    SHAPE ABUTMENT ;
    PORT
      LAYER metal1 ;
        RECT 0 1.315 0.38 1.485 ;
    END
  END VDD
  OBS
    LAYER metal1 ;
      RECT 0.1 0.9 0.15 1.0 ;
  END
END INV_X1
MACRO FILL1
  CLASS CORE SPACER ;
  SIZE 0.19 BY 1.4 ;
  SITE core ;
END FILL1
END LIBRARY
)";

static const char* DEF = R"(
VERSION 5.8 ;
DIVIDERCHAR "/" ;
BUSBITCHARS "[]" ;
DESIGN top ;
UNITS DISTANCE MICRONS 2000 ;
DIEAREA ( 0 0 ) ( 20000 14000 ) ;
ROW ROW_0 core 380 2800 N DO 50 BY 1 STEP 380 0 ;
ROW ROW_1 core 380 5600 FS DO 50 BY 1 STEP 380 0 ;
TRACKS X 190 DO 52 STEP 380 LAYER metal2 ;
TRACKS Y 140 DO 50 STEP 280 LAYER metal1 ;
GCELLGRID X 0 DO 5 STEP 4000 ;
NONDEFAULTRULES 1 ;
- wide + LAYER metal2 WIDTH 280 SPACING 280 ;
END NONDEFAULTRULES
VIAS 1 ;
- gen2x1 + VIARULE Via1Array + CUTSIZE 140 140 + LAYERS metal1 via1 metal2
  + CUTSPACING 160 160 + ENCLOSURE 70 0 0 70 + ROWCOL 1 2 ;
END VIAS
COMPONENTS 2 ;
- u1 INV_X1 + PLACED ( 380 2800 ) N ;
- u2/x\[3\] INV_X1 + SOURCE TIMING + FIXED ( 1140 5600 ) FS ;
END COMPONENTS
PINS 2 ;
- a + NET a + DIRECTION INPUT + USE SIGNAL
  + LAYER metal2 ( -70 0 ) ( 70 280 ) + PLACED ( 950 0 ) N ;
- y[0] + NET y[0] + DIRECTION OUTPUT + USE SIGNAL
  + PORT + LAYER metal2 ( -70 0 ) ( 70 280 ) + PLACED ( 1330 14000 ) S
  + PORT + LAYER metal1 ( 0 -70 ) ( 280 70 ) + PLACED ( 0 700 ) N ;
END PINS
BLOCKAGES 2 ;
- LAYER metal2 RECT ( 4000 4000 ) ( 6000 6000 ) ;
- PLACEMENT RECT ( 8000 2800 ) ( 9000 4200 ) ;
END BLOCKAGES
SPECIALNETS 1 ;
- VDD ( * VDD ) + USE POWER
  + ROUTED metal1 340 + SHAPE FOLLOWPIN ( 380 5600 ) ( 19380 * )
    NEW metal2 800 + SHAPE STRIPE ( 2000 0 ) ( * 14000 )
    NEW metal1 0 ( 2000 5600 ) gen2x1 DO 2 BY 1 STEP 4000 0 ;
END SPECIALNETS
NETS 3 ;
- a ( PIN a ) ( u1 A ) + USE SIGNAL
  + ROUTED metal2 ( 950 140 ) ( * 4060 ) via1_0
    NEW metal1 ( 570 4060 0 ) ( 950 * 35 ) ;
- n1 ( u1 ZN ) ( u2/x\[3\] A ) + NONDEFAULTRULE wide
  + ROUTED metal2 ( 1330 3500 ) ( * 6300 ) NEW metal2 TAPER ( 1330 6300 ) ( * 6580 ) ;
- y[0] ( u2/x\[3\] ZN ) ( PIN y[0] ) ;
END NETS
END DESIGN
)";

static void put(const char* path, const char* text) {
  FILE* f = fopen(path, "w");
  assert(f);
  fputs(text, f);
  fclose(f);
}

static void checkDesign(const phys::Design& d) {
  assert(d.name == "top" && d.dbu == 2000);
  assert(d.die.x1 == 20000 && d.die.y1 == 14000);
  assert(d.rows.size() == 2 && d.rows[1].orient == "FS" && d.rows[0].nx == 50 && d.rows[0].stepX == 380);
  assert(d.tracks.size() == 2 && d.tracks[0].axis == 'X' && d.tracks[0].layers[0] == "metal2" && d.tracks[1].step == 280);
  assert(d.gcells.size() == 1 && d.gcells[0].count == 5);
  assert(d.ndrs.at("wide").at("metal2").width == 280);
  assert(d.comps.size() == 2 && d.comps[0].status == "PLACED" && d.comps[0].x == 380);
  assert(d.comps[1].name == "u2/x\\[3\\]" && d.comps[1].status == "FIXED" && d.comps[1].orient == "FS");
  assert(d.pins.size() == 2 && d.pins[0].layer == "metal2" && d.pins[0].x == 950);
  phys::Rect ps = d.pins[0].shape();
  assert(ps.x0 == 880 && ps.x1 == 1020 && ps.y0 == 0 && ps.y1 == 280);
  assert(d.pins[1].name == "y[0]" && d.pins[1].orient == "S");
  phys::Rect top = d.pins[1].shape();          // rotated 180: hangs below the top edge
  assert(top.y0 == 14000 - 280 && top.y1 == 14000 && top.x0 == 1260);
  assert(d.blockages.size() == 2 && d.blockages[0].layer == "metal2" && d.blockages[1].layer.empty());
  assert(d.specialNets.size() == 1);
  const phys::Net& vdd = d.specialNets[0];
  assert(vdd.use == "POWER" && vdd.segs.size() == 2 && vdd.segs[0].width == 340 && vdd.segs[0].shape == "FOLLOWPIN");
  assert(vdd.segs[0].x1 == 19380 && vdd.segs[0].y1 == 5600 && vdd.segs[1].shape == "STRIPE" && vdd.segs[1].y1 == 14000);
  assert(vdd.vias.size() == 2 && vdd.vias[1].x == 6000);
  assert(d.nets.size() == 3);
  const phys::Net& a = d.nets[0];
  assert(a.conns.size() == 2 && a.conns[0].inst == "PIN" && a.conns[1].pin == "A");
  assert(a.wiring == "ROUTED" && a.segs.size() == 2 && a.vias.size() == 1);
  assert(a.segs[0].layer == "metal2" && a.segs[0].x1 == 950 && a.segs[0].y1 == 4060);
  assert(a.vias[0].via == "via1_0" && a.vias[0].y == 4060);
  assert(a.segs[1].layer == "metal1" && a.segs[1].x0 == 570 && a.segs[1].ext0 == 0 && a.segs[1].ext1 == 35);
  const phys::Net& n1 = d.nets[1];
  assert(n1.ndr == "wide" && n1.segs.size() == 2 && n1.segs[0].width == 280 && n1.segs[1].taper && n1.segs[1].width == 0);
  assert(d.nets[2].wiring.empty() && d.nets[2].segs.empty());
}

int main() {
  put("lefdef_t.lef", LEF);
  put("lefdef_t.def", DEF);
  phys::Tech t;
  phys::readLef("lefdef_t.lef", t);
  assert(t.dbu == 2000 && t.mfgGrid == 10);
  assert(t.layers.size() == 3);
  const phys::Layer* m1 = t.layer("metal1");
  assert(m1 && m1->type == phys::Layer::Routing && m1->dir == 'H' && m1->routingLevel == 1);
  assert(m1->width == 140 && m1->spacing == 130 && m1->pitch() == 280 && m1->offsetX == 190 && m1->offsetY == 140);
  assert(m1->rPerSq == 0.38 && m1->cPerSqDist == 7.7e-05 && m1->edgeCap == 2.7e-05 && m1->height == -1);
  const phys::Layer* v1 = t.layer("via1");
  assert(v1->type == phys::Layer::Cut && v1->cutRes == 5 && v1->rPerSq == -1);
  const phys::Layer* m2 = t.layer("metal2");
  assert(m2->dir == 'V' && m2->spacing == 140 && m2->pitchX == 380 && m2->pitchY == 280 && m2->routingLevel == 2);
  assert(t.routing().size() == 2 && t.cutBetween(*m1, *m2) == v1);
  const phys::ViaDef* via = t.viaBetween("metal1", "metal2");
  assert(via && via->name == "via1_0" && via->isDefault && via->shapes.size() == 3 && via->res == -1);
  assert(t.ndrs.at("wide").at("metal2").width == 280);
  assert(t.coreSite() && t.coreSite()->w == 380 && t.coreSite()->h == 2800);
  const phys::Macro* inv = t.macro("INV_X1");
  assert(inv && inv->w == 760 && inv->h == 2800 && inv->isCore() && !inv->isFiller() && inv->symX && inv->symY);
  assert(inv->pins.size() == 3 && inv->pin("A")->dir == "INPUT" && inv->pin("VDD")->use == "POWER");
  assert(inv->pin("A")->ports[0].r.x0 == 120 && inv->pin("A")->ports[0].r.y1 == 1400);
  // The polygon is three slabs whose area is that of the outline.
  const auto& zn = inv->pin("ZN")->ports;
  int64_t area = 0;
  for (auto& s : zn) area += s.r.w() * s.r.h();
  assert(zn.size() == 3 && area == 140 * 2200 + 60 * 600);
  assert(inv->obs.size() == 1 && t.macro("FILL1")->isFiller());

  // Orientation: FS mirrors about the x axis, S turns the cell upside down.
  phys::Rect r{120, 1050, 330, 1400};
  phys::Rect fs = phys::xform(r, "FS", 760, 2800, 1000, 5600);
  assert(fs.x0 == 1120 && fs.x1 == 1330 && fs.y0 == 5600 + 1400 && fs.y1 == 5600 + 1750);
  phys::Rect s = phys::xform(r, "S", 760, 2800, 0, 0);
  assert(s.x0 == 430 && s.x1 == 640 && s.y0 == 1400 && s.y1 == 1750);
  phys::Rect w = phys::xform(r, "W", 760, 2800, 0, 0);     // 90 degrees counterclockwise
  assert(w.x0 == 1400 && w.x1 == 1750 && w.y0 == 120 && w.y1 == 330);

  phys::Design d;
  phys::readDef("lefdef_t.def", d);
  checkDesign(d);
  assert(d.vias.size() == 1 && d.vias[0].shapes.size() == 4);          // bottom, two cuts, top
  assert(d.vias[0].shapes[0].r.x0 == -220 - 70 && d.vias[0].shapes[0].r.x1 == 220 + 70 && d.vias[0].shapes[3].r.y1 == 70 + 70);
  assert(d.pins[1].shapes.size() == 2 && d.pins[1].shapes[1].layer == "metal1" && d.pins[1].shapes[1].r.y0 == 630);

  phys::writeDef("lefdef_t.out.def", d);
  phys::Design e;
  phys::readDef("lefdef_t.out.def", e);
  checkDesign(e);

  bool threw = false;
  put("lefdef_bad.def", "DESIGN x ;\nUNITS DISTANCE MICRONS oops ;\n");
  try { phys::Design b; phys::readDef("lefdef_bad.def", b); }
  catch (const std::runtime_error& ex) { threw = std::string(ex.what()).find("lefdef_bad.def:2") != std::string::npos; }
  assert(threw);
  std::cout << "test_lefdef: ok\n";
}
