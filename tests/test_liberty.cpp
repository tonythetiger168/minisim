// The Liberty reader (src/liberty.hpp) on tests/sta/sta_test.lib and on
// inline snippets: syntax, units, tables, arcs, ff/latch groups, buses,
// function expressions, power and wire load groups.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>

#include "liberty.hpp"

static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

int main(int argc, char** argv) {
    assert(argc > 1);
    liberty::Library L = liberty::load(argv[1]);
    assert(L.name == "sta_test");
    assert(near(L.timeScale, 1) && near(L.capScale, 1));
    assert(near(L.defaultMaxTransition, 1.0));
    assert(L.cell("INV_X1") && L.cell("DFF_X1") && !L.cell("NOPE"));

    // INV: one negative-unate arc; the tables are linear, so interpolation
    // and extrapolation both give the closed form.
    const liberty::Cell& inv = *L.cell("INV_X1");
    assert(inv.pins.size() == 2 && inv.arcs.size() == 1 && !inv.sequential());
    const liberty::Arc& a = inv.arcs[0];
    assert(a.related == "A" && a.to == "Y" && a.sense == liberty::Sense::Negative);
    assert(a.type == liberty::ArcType::Comb);
    assert(near(a.cellRise.lookup(0.01, 0.001), 0.05 + 0.2 * 0.01 + 2.0 * 0.001));
    assert(near(a.cellRise.lookup(0.2, 0.03), 0.05 + 0.2 * 0.2 + 2.0 * 0.03));    // inside
    assert(near(a.cellRise.lookup(1.0, 0.3), 0.05 + 0.2 * 1.0 + 2.0 * 0.3));      // beyond both
    assert(near(a.cellFall.lookup(0.0, 0.0), 0.045));                              // below both
    assert(near(a.riseTrans.lookup(0.1, 0.01), 0.02 + 0.01 + 0.03));
    assert(near(inv.pin("A")->cap, 0.002) && inv.pin("Y")->dir == liberty::Dir::Out);
    assert(near(inv.pin("Y")->maxCapacitance, 0.2));

    // DFF: ff group, an edge arc, setup and hold checks.
    const liberty::Cell& ff = *L.cell("DFF_X1");
    assert(ff.sequential() && ff.ffs.size() == 1);
    assert(ff.ffs[0].iq == "IQ" && ff.ffs[0].iqn == "IQN" && ff.ffs[0].clockedOn == "CK" && ff.ffs[0].nextState == "D");
    assert(ff.pin("CK")->clock);
    int setup = 0, hold = 0, edge = 0;
    for (auto& x : ff.arcs) {
        if (x.type == liberty::ArcType::SetupRising) {
            setup++;
            assert(x.isCheck() && x.isSetup() && x.rising() && x.to == "D" && x.related == "CK");
            // rise_constraint = 0.05 + 0.1*clk slew + 0.2*data slew
            assert(near(x.riseCons.constraint(0.3, 0.2), 0.05 + 0.1 * 0.2 + 0.2 * 0.3));
        }
        if (x.type == liberty::ArcType::HoldRising) hold++;
        if (x.type == liberty::ArcType::RisingEdge) { edge++; assert(x.isEdge() && x.to == "Q"); }
    }
    assert(setup == 1 && hold == 1 && edge == 1);
    assert(L.cell("DFFN_X1")->ffs[0].clockedOn == "!CK");
    const liberty::Cell& ffr = *L.cell("DFFR_X1");
    assert(ffr.ffs[0].clear == "!RN");
    bool clr = false, rec = false;
    for (auto& x : ffr.arcs) {
        if (x.typeName == "clear") { clr = true; assert(x.fallOnly && x.cellRise.empty() && !x.cellFall.empty()); }
        if (x.type == liberty::ArcType::RecoveryRising) rec = true;
    }
    assert(clr && rec);
    const liberty::Cell& lat = *L.cell("LATCH_X1");
    assert(lat.latches.size() == 1 && lat.latches[0].enable == "G" && lat.latches[0].dataIn == "D");

    // Buses expand to bits; a bit's own pin group overrides the bus.
    const liberty::Cell& reg = *L.cell("REG4_X1");
    assert(reg.dontUse && reg.pins.size() == 9);
    assert(reg.pin("D[3]") && reg.pin("D[0]") && reg.pin("D[2]")->bus == "D" && reg.pin("D[2]")->busBit == 2);
    assert(near(reg.pin("D[1]")->cap, 0.002) && reg.pin("Q[0]")->dir == liberty::Dir::Out);
    assert(near(reg.pin("Q[3]")->maxCapacitance, 0.05) && reg.pin("Q[2]")->maxCapacitance < 0);

    // Function expressions.
    using liberty::parseFunction;
    assert(parseFunction("!(A & B)").truthTable({"A", "B"}) == 0x7);
    assert(parseFunction("(A*B)'").truthTable({"A", "B"}) == 0x7);
    assert(parseFunction("A B + C").truthTable({"A", "B", "C"}) == 0xF8);
    assert(parseFunction("A ^ B & C").truthTable({"A", "B", "C"}) == 0x60);   // ^ binds tighter than &
    assert(parseFunction("A' + 0").truthTable({"A"}) == 0x1);
    assert(parseFunction("!A !B | 1").truthTable({"A", "B"}) == 0xF);
    assert(L.cell("MUX2_X1")->pin("Y")->functionExpr().truthTable({"A", "B", "S"}) == 0xCA);
    assert((parseFunction("(A & !S) | (B & S)").vars() == std::vector<std::string>{"A", "S", "B"}));
    bool threw = false;
    try { parseFunction("A & (B"); } catch (liberty::Error&) { threw = true; }
    assert(threw);

    // Units, scalar tables, 1-D tables, a missing ';', power and wire load.
    const char* src = R"(
library (u) {
  time_unit : "10ps";
  capacitive_load_unit (1, ff);
  leakage_power_unit : "1nW";
  default_wire_load : "small"
  wire_load ("small") {
    resistance : 0.1; capacitance : 0.2; slope : 2;
    fanout_length (1, 10);
    fanout_length (3, 30);
  }
  lu_table_template (t1) { variable_1 : total_output_net_capacitance; index_1 ("1, 11"); }
  power_lut_template (p1) { variable_1 : input_transition_time; index_1 ("1, 2"); }
  cell (X) {
    area : 3.5;
    cell_leakage_power : 7;
    leakage_power () { when : "!A"; value : 5; }
    pin (A) { direction : input; capacitance : 2;
      internal_power () { rise_power (p1) { values ("4, 6"); } fall_power (scalar) { values ("9"); } } }
    pin (Y) { direction : output; function : "A";
      timing () { related_pin : "A"; timing_sense : positive_unate;
        cell_rise (t1) { values ("10, 30"); }
        cell_fall (scalar) { values ("5"); }
        rise_transition (t1) { index_1 ("0, 10"); values ("1, 2"); }
      }
    }
    test_cell () { pin (SI) { direction : input; signal_type : test_scan_in; } }
  }
}
)";
    liberty::Library U = liberty::Library::load(liberty::parse(src, "u.lib"), "u.lib");
    assert(near(U.timeScale, 0.01) && near(U.capScale, 0.001));
    const liberty::Cell& x = *U.cell("X");
    assert(near(x.area, 3.5) && near(x.pin("A")->cap, 0.002));
    const liberty::Arc& xa = x.arcs[0];
    // load 6 fF = 0.006 pF, halfway between 1 fF and 11 fF: 20 units = 0.2 ns
    assert(near(xa.cellRise.lookup(0.0, 0.006), 0.2));
    assert(near(xa.cellFall.lookup(0.3, 0.5), 0.05));
    assert(near(xa.riseTrans.lookup(0.0, 0.005), 0.015));
    assert(near(x.leakage, 7) && x.leakages.size() == 1 && x.leakages[0].when == "!A" && near(x.leakages[0].value, 5));
    assert(x.pin("A")->power.size() == 1);
    assert(near(x.pin("A")->power[0].risePower.lookup(0.015, 0), 5));   // slew 1.5 units
    assert(near(x.pin("A")->power[0].fallPower.lookup(0.5, 0), 9));
    assert(x.testPins.size() == 1 && x.testPins[0].signalType == "test_scan_in");
    assert(U.defaultWireLoad == "small" && U.leakagePowerUnit == "1nW");
    const liberty::WireLoad& w = *U.wireLoad("small");
    assert(near(w.length(1), 10) && near(w.length(2), 20) && near(w.length(5), 34));
    // The generic tree keeps what the model does not.
    assert(U.root->first("cell")->first("test_cell") != nullptr);

    // Errors carry file:line.
    threw = false;
    try { liberty::parse("library (e) {\n cell (c) {\n", "e.lib"); }
    catch (liberty::Error& e) { threw = std::string(e.what()).find("e.lib:") == 0; }
    assert(threw);
    threw = false;
    try {
        liberty::Library::load(liberty::parse(
            "library (e) { lu_table_template (t) { variable_1 : input_net_transition; index_1 (\"1,2,3\"); }\n"
            "cell (c) { pin (y) { direction : output; timing () { related_pin : \"a\";\n"
            "cell_rise (t) { values (\"1, 2\"); } } } } }", "e.lib"), "e.lib");
    } catch (liberty::Error& e) { threw = std::string(e.what()).find("table has 2 values") != std::string::npos; }
    assert(threw);
    // Writer: the text parses back to the same tree, for the whole test
    // library and for the awkward values (quotes, expressions, bus bits,
    // multi-row tables, empty argument lists).
    {
        liberty::Library W = liberty::load(argv[1]);
        std::string text = liberty::write(*W.root);
        liberty::Group back = liberty::parse(text, "written.lib");
        assert(liberty::sameTree(*W.root, back));
        assert(liberty::write(back) == text);
        liberty::Library W2 = liberty::Library::load(back, "written.lib");
        assert(W2.cells.size() == W.cells.size());
        for (size_t i = 0; i < W.cells.size(); i++)
            assert(W2.cells[i].name == W.cells[i].name && W2.cells[i].arcs.size() == W.cells[i].arcs.size());

        liberty::Group g = liberty::parse(
            "library (w) {\n  time_unit : \"1ns\";\n  capacitive_load_unit (1, ff);\n  nom_voltage : 1.1;\n"
            "  cell (\"A[1]\") {\n    pin (Y) { direction : output; function : \"!(A*B)\"; x_expr : 1.2 * VDD;\n"
            "      timing () { related_pin : \"A\"; values (\"1, 2\", \\\n \"3, 4\"); index_1 (\"0.1, 0.2\"); }\n    }\n  }\n}\n");
        std::string t = liberty::write(g);
        assert(t.find("time_unit : \"1ns\";") != std::string::npos);
        assert(t.find("capacitive_load_unit (1, ff);") != std::string::npos);
        assert(t.find("nom_voltage : 1.1;") != std::string::npos);
        assert(t.find("cell (\"A[1]\") {") != std::string::npos);
        assert(t.find("direction : output;") != std::string::npos);
        assert(t.find("function : \"!(A*B)\";") != std::string::npos);
        assert(t.find("x_expr : \"1.2 * VDD\";") != std::string::npos);
        assert(t.find("timing () {") != std::string::npos);
        assert(t.find("values (\"1, 2\", \\\n") != std::string::npos);
        assert(t.find("index_1 (\"0.1, 0.2\");") != std::string::npos);
        assert(liberty::sameTree(g, liberty::parse(t)));
        liberty::Group other = g;
        other.groups[0].groups[0].attrs[1].vals[0] = "A*B";
        assert(!liberty::sameTree(g, other));
    }
    // Units, operating conditions, supplies and thresholds.
    {
        liberty::Library O = liberty::Library::load(liberty::parse(
            "library (o) {\n  voltage_unit : \"10mV\";\n  current_unit : \"1uA\";\n  pulling_resistance_unit : \"1ohm\";\n"
            "  leakage_power_unit : \"1pW\";\n  nom_voltage : 110;\n  nom_process : 1.2;\n"
            "  voltage_map (VDD, 110);\n  voltage_map (VSS, 0);\n"
            "  operating_conditions (slow) { process : 1.2; voltage : 95; temperature : 125; }\n"
            "  operating_conditions (fast) { process : 0.8; voltage : 125; temperature : -40; }\n"
            "  default_operating_conditions : fast;\n"
            "  slew_lower_threshold_pct_rise : 10;\n  slew_upper_threshold_pct_fall : 90;\n  input_threshold_pct_fall : 40;\n"
            "  slew_derate_from_library : 0.5;\n}\n"), "o.lib");
        assert(near(O.voltageScale, 0.01) && near(O.currentScale, 1e-3) && near(O.resistanceScale, 1e-3) && near(O.leakageScale, 1e-3));
        assert(near(O.nomVoltage, 1.1) && near(O.nomProcess, 1.2));
        assert(O.voltageMap.size() == 2 && near(O.voltageMap.at("VDD"), 1.1) && near(O.voltageMap.at("VSS"), 0));
        assert(O.operatingConditions.size() == 2 && O.conditions("slow") && near(O.conditions("slow")->voltage, 0.95));
        assert(near(O.conditions("slow")->temperature, 125) && near(O.conditions("slow")->process, 1.2));
        assert(O.conditions() == O.conditions("fast") && near(O.conditions()->temperature, -40));
        assert(O.conditions("none") == nullptr);
        assert(near(O.slewLowerRise, 10) && near(O.slewUpperRise, 80) && near(O.slewLowerFall, 20) && near(O.slewUpperFall, 90));
        assert(near(O.inputThresholdRise, 50) && near(O.inputThresholdFall, 40) && near(O.outputThresholdRise, 50));
        assert(near(O.slewDerate, 0.5));
        assert(near(liberty::Library::unitScale("1kohm", 1e3), 1) && near(liberty::Library::unitScale("1V", 1), 1));
        assert(near(liberty::Library::unitScale("1mW", 1e-9), 1e6) && near(liberty::Library::unitScale("1nW", 1e-9), 1));
        // Nothing given: defaults, and no conditions.
        liberty::Library D = liberty::Library::load(liberty::parse("library (d) { }"), "d.lib");
        assert(D.conditions() == nullptr && near(D.voltageScale, 1) && near(D.slewLowerRise, 20) && near(D.slewUpperFall, 80));
    }
    std::puts("test_liberty: ok");
    return 0;
}
